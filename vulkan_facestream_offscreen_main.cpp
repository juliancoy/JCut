#include "facestream_generation.h"
#include "facestream_runtime.h"
#include "facestream_tracking.h"
#include "clip_serialization.h"
#include "detector_settings.h"
#include "decoder_context.h"
#include "decoder_ffmpeg_utils.h"
#include "debug_controls.h"
#include "editor_shared.h"
#include "frame_handle.h"
#include "imgui_preview_window.h"
#include "json_io_utils.h"
#include "vulkan_res10_ncnn_face_detector.h"
#include "vulkan_detector_frame_handoff.h"
#include "vulkan_scrfd_ncnn_face_detector.h"
#include "vulkan_facestream_offscreen_runner.h"
#include "vulkan_zero_copy_face_detector.h"

#include <QDir>
#include <QDataStream>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QCheckBox>
#include <QComboBox>
#include <QDebug>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLocalSocket>
#include <QPainter>
#include <QPushButton>
#include <QProgressBar>
#include <QSet>
#include <QSlider>
#include <QString>
#include <QStringList>
#include <QtGlobal>
#include <QVBoxLayout>
#include <QWidget>

#include <vulkan/vulkan.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <fcntl.h>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

#if JCUT_HAVE_OPENCV
#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/objdetect.hpp>
#endif

namespace {

struct Options {
    QString videoPath = QStringLiteral("/mnt/Cancer/PanelVid2TikTok/Politics/YTDown.com_YouTube_Meet-the-Candidates-for-Baltimore-County_Media_Hho5MORgIj8_001_1080p.mp4");
    QString outputDir = QStringLiteral("testbench_assets/vulkan_facestream_offscreen");
    int maxFrames = 0; // 0 => full video
    int startFrame = 0;
    int stride = jcut::facestream::kDefaultDetectorStride;
    int maxDetections = jcut::facestream::kDefaultDetectorMaxDetections;
    int maxFacesPerFrame = jcut::facestream::kDefaultDetectorMaxFacesPerFrame; // 0 => no post-cap
    int scrfdTargetSize = jcut::facestream::kDefaultDetectorScrfdTargetSize;
    QString scrfdModelVariant = QString::fromLatin1(jcut::facestream::kDefaultScrfdModelVariant);
    int previewFrames = 24;
    int previewStride = 12;
    float threshold = jcut::facestream::kDefaultDetectorThreshold;
    float nmsIouThreshold = jcut::facestream::kDefaultDetectorNmsIouThreshold;
    float trackMatchIouThreshold = jcut::facestream::kDefaultDetectorTrackMatchIouThreshold;
    float newTrackMinConfidence = jcut::facestream::kDefaultDetectorNewTrackMinConfidence;
    bool primaryFaceOnly = jcut::facestream::kDefaultDetectorPrimaryFaceOnly;
    bool smallFaceFallback = jcut::facestream::kDefaultDetectorSmallFaceFallback;
    bool scrfdTiled = jcut::facestream::kDefaultDetectorScrfdTiled;
    QString detector = QStringLiteral("jcut-dnn");
    QString res10ParamPath;
    QString res10BinPath;
    QString scrfdParamPath;
    QString scrfdBinPath;
    QString paramsFile;
    QString clipJsonPath;
    QString previewSocket;
    bool livePreview = false;
    bool writePreviewFiles = false;
    bool applyClipGrading = false;
    bool materializedGenerateFacestream = false;
    bool heuristicZeroCopyDetector = false;
    bool scrfdDetector = true;
    bool requireHardwareVulkanFramePath = false;
    bool requireZeroCopy = false;
    bool allowCpuUploadFallback = false;
    bool verbose = false;
    bool progress = isatty(STDOUT_FILENO);
    int logInterval = 0; // 0 => summary only unless --verbose is set
    bool preflight = false;
    editor::DecodePreference decodePreference = editor::DecodePreference::HardwareZeroCopy;
};

class ScopedStderrSilencer {
public:
    explicit ScopedStderrSilencer(bool enabled)
    {
        if (!enabled) {
            return;
        }
        fflush(stderr);
        m_savedFd = dup(STDERR_FILENO);
        if (m_savedFd < 0) {
            return;
        }
        const int nullFd = open("/dev/null", O_WRONLY);
        if (nullFd < 0) {
            close(m_savedFd);
            m_savedFd = -1;
            return;
        }
        if (dup2(nullFd, STDERR_FILENO) < 0) {
            close(nullFd);
            close(m_savedFd);
            m_savedFd = -1;
            return;
        }
        close(nullFd);
    }

    ~ScopedStderrSilencer()
    {
        if (m_savedFd >= 0) {
            fflush(stderr);
            dup2(m_savedFd, STDERR_FILENO);
            close(m_savedFd);
        }
    }

    ScopedStderrSilencer(const ScopedStderrSilencer&) = delete;
    ScopedStderrSilencer& operator=(const ScopedStderrSilencer&) = delete;

private:
    int m_savedFd = -1;
};

struct DetectionSanitizeStats {
    int rawCount = 0;
    int rejectedInvalidConfidence = 0;
    int rejectedDegenerate = 0;
    int rejectedRoi = 0;
    int rejectedArea = 0;
    int rejectedAspect = 0;
    int rejectedNms = 0;
    int keptBeforeNms = 0;
    int keptAfterNms = 0;
};

using Detection = jcut::facestream::Detection;
using Track = jcut::facestream::ContinuityTrack;

QString backendIdForOptions(const Options& options)
{
    if (options.materializedGenerateFacestream) {
        return QStringLiteral("materialized_generate_facestream_opencv_cascade_v1");
    }
    if (options.heuristicZeroCopyDetector) {
        return QStringLiteral("native_jcut_heuristic_zero_copy_v1");
    }
    if (options.scrfdDetector) {
        return QStringLiteral("scrfd_%1_ncnn_vulkan_zero_copy_v1")
            .arg(jcut::facestream::normalizeScrfdModelVariantId(options.scrfdModelVariant));
    }
    return QStringLiteral("res10_ssd_ncnn_vulkan_zero_copy_v1");
}

struct RuntimeTuning {
    int stride = jcut::facestream::kDefaultDetectorStride;
    int maxDetections = jcut::facestream::kDefaultDetectorMaxDetections;
    int scrfdTargetSize = jcut::facestream::kDefaultDetectorScrfdTargetSize;
    int maxFacesPerFrame = jcut::facestream::kDefaultDetectorMaxFacesPerFrame;
    QString scrfdModelVariant = QString::fromLatin1(jcut::facestream::kDefaultScrfdModelVariant);
    float threshold = jcut::facestream::kDefaultDetectorThreshold;
    float nmsIouThreshold = jcut::facestream::kDefaultDetectorNmsIouThreshold;
    float trackMatchIouThreshold = jcut::facestream::kDefaultDetectorTrackMatchIouThreshold;
    float newTrackMinConfidence = jcut::facestream::kDefaultDetectorNewTrackMinConfidence;
    bool primaryFaceOnly = jcut::facestream::kDefaultDetectorPrimaryFaceOnly;
    bool smallFaceFallback = jcut::facestream::kDefaultDetectorSmallFaceFallback;
    bool scrfdTiled = jcut::facestream::kDefaultDetectorScrfdTiled;
    float roiX1 = jcut::facestream::kDefaultDetectorRoiX1;
    float roiY1 = jcut::facestream::kDefaultDetectorRoiY1;
    float roiX2 = jcut::facestream::kDefaultDetectorRoiX2;
    float roiY2 = jcut::facestream::kDefaultDetectorRoiY2;
    float minFaceAreaRatio = jcut::facestream::kDefaultDetectorMinFaceAreaRatio;
    float maxFaceAreaRatio = jcut::facestream::kDefaultDetectorMaxFaceAreaRatio;
    float minAspect = jcut::facestream::kDefaultDetectorMinAspect;
    float maxAspect = jcut::facestream::kDefaultDetectorMaxAspect;
};

struct PreviewDebugSettings {
    bool followLatest = true;
    bool showDetections = true;
    bool showTracks = true;
    bool showLabels = true;
    bool showConfirmedTracks = true;
    bool showTentativeTracks = true;
    bool showLostTracks = true;
    float playbackSpeed = 1.0f;
    float detectionLineThickness = 1.5f;
    float trackLineThickness = 2.5f;
    float overlayOpacity = 1.0f;
};

jcut::facestream::ContinuityTrackingTuning trackingTuningForRuntime(
    const RuntimeTuning& tuning)
{
    jcut::facestream::ContinuityTrackingTuning trackingTuning;
    trackingTuning.trackMatchIouThreshold = tuning.trackMatchIouThreshold;
    trackingTuning.newTrackMinConfidence = tuning.newTrackMinConfidence;
    trackingTuning.primaryFaceOnly = tuning.primaryFaceOnly;
    trackingTuning.staleTrackFrameWindow = 48;
    return trackingTuning;
}

QJsonObject buildRawDetectionFrameRecord(int frameNumber,
                                         const QString& detectorId,
                                         const QSize& frameSize,
                                         const QVector<Detection>& detections)
{
    QJsonArray detectionRows;
    for (const Detection& detection : detections) {
        detectionRows.append(jcut::facestream::compactDetectionJson(detection, frameSize));
    }
    return QJsonObject{
        {QStringLiteral("frame"), frameNumber},
        {QStringLiteral("detector"), detectorId},
        {QStringLiteral("frame_width"), frameSize.width()},
        {QStringLiteral("frame_height"), frameSize.height()},
        {QStringLiteral("detection_count"), detections.size()},
        {QStringLiteral("detections"), detectionRows}
    };
}

QString detectorSettingsPathForVideo(const QString& videoPath)
{
    const QFileInfo info(videoPath);
    const QString baseName = info.completeBaseName().isEmpty()
        ? info.fileName()
        : info.completeBaseName();
    return info.dir().absoluteFilePath(baseName + QStringLiteral("_detectorsettings.json"));
}

QJsonObject runtimeTuningToJson(const RuntimeTuning& tuning,
                                const QString& detector,
                                int scrfdTargetSize)
{
    return {
        {QStringLiteral("detector"), detector},
        {QStringLiteral("scrfd_model_variant"), jcut::facestream::normalizeScrfdModelVariantId(tuning.scrfdModelVariant)},
        {QStringLiteral("scrfd_target_size"), tuning.scrfdTargetSize > 0 ? tuning.scrfdTargetSize : scrfdTargetSize},
        {QStringLiteral("stride"), tuning.stride},
        {QStringLiteral("max_detections"), tuning.maxDetections},
        {QStringLiteral("max_faces_per_frame"), tuning.maxFacesPerFrame},
        {QStringLiteral("threshold"), tuning.threshold},
        {QStringLiteral("nms_iou_threshold"), tuning.nmsIouThreshold},
        {QStringLiteral("track_match_iou_threshold"), tuning.trackMatchIouThreshold},
        {QStringLiteral("new_track_min_confidence"), tuning.newTrackMinConfidence},
        {QStringLiteral("primary_face_only"), tuning.primaryFaceOnly},
        {QStringLiteral("small_face_fallback"), tuning.smallFaceFallback},
        {QStringLiteral("scrfd_tiled"), tuning.scrfdTiled},
        {QStringLiteral("roi_x1"), tuning.roiX1},
        {QStringLiteral("roi_y1"), tuning.roiY1},
        {QStringLiteral("roi_x2"), tuning.roiX2},
        {QStringLiteral("roi_y2"), tuning.roiY2},
        {QStringLiteral("min_face_area_ratio"), tuning.minFaceAreaRatio},
        {QStringLiteral("max_face_area_ratio"), tuning.maxFaceAreaRatio},
        {QStringLiteral("min_aspect"), tuning.minAspect},
        {QStringLiteral("max_aspect"), tuning.maxAspect}
    };
}

QJsonObject previewDebugSettingsToJson(const PreviewDebugSettings& settings)
{
    return {
        {QStringLiteral("follow_latest"), settings.followLatest},
        {QStringLiteral("show_detections"), settings.showDetections},
        {QStringLiteral("show_tracks"), settings.showTracks},
        {QStringLiteral("show_labels"), settings.showLabels},
        {QStringLiteral("show_confirmed_tracks"), settings.showConfirmedTracks},
        {QStringLiteral("show_tentative_tracks"), settings.showTentativeTracks},
        {QStringLiteral("show_lost_tracks"), settings.showLostTracks},
        {QStringLiteral("playback_speed"), settings.playbackSpeed},
        {QStringLiteral("detection_line_thickness"), settings.detectionLineThickness},
        {QStringLiteral("track_line_thickness"), settings.trackLineThickness},
        {QStringLiteral("overlay_opacity"), settings.overlayOpacity}
    };
}

void applyPreviewDebugSettingsObject(const QJsonObject& object, PreviewDebugSettings* settings)
{
    if (!settings) {
        return;
    }
    settings->followLatest = object.value(QStringLiteral("follow_latest")).toBool(settings->followLatest);
    settings->showDetections = object.value(QStringLiteral("show_detections")).toBool(settings->showDetections);
    settings->showTracks = object.value(QStringLiteral("show_tracks")).toBool(settings->showTracks);
    settings->showLabels = object.value(QStringLiteral("show_labels")).toBool(settings->showLabels);
    settings->showConfirmedTracks =
        object.value(QStringLiteral("show_confirmed_tracks")).toBool(settings->showConfirmedTracks);
    settings->showTentativeTracks =
        object.value(QStringLiteral("show_tentative_tracks")).toBool(settings->showTentativeTracks);
    settings->showLostTracks =
        object.value(QStringLiteral("show_lost_tracks")).toBool(settings->showLostTracks);
    settings->playbackSpeed = std::clamp(
        static_cast<float>(object.value(QStringLiteral("playback_speed")).toDouble(settings->playbackSpeed)),
        0.25f,
        4.0f);
    settings->detectionLineThickness = std::clamp(
        static_cast<float>(object.value(QStringLiteral("detection_line_thickness"))
                               .toDouble(settings->detectionLineThickness)),
        1.0f,
        4.0f);
    settings->trackLineThickness = std::clamp(
        static_cast<float>(object.value(QStringLiteral("track_line_thickness"))
                               .toDouble(settings->trackLineThickness)),
        1.0f,
        5.0f);
    settings->overlayOpacity = std::clamp(
        static_cast<float>(object.value(QStringLiteral("overlay_opacity")).toDouble(settings->overlayOpacity)),
        0.2f,
        1.0f);
}

bool saveRuntimeTuningFile(const QString& path,
                           const RuntimeTuning& tuning,
                           const QString& detector,
                           int scrfdTargetSize,
                           const PreviewDebugSettings* previewSettings,
                           QString* errorMessage = nullptr)
{
    if (path.isEmpty()) {
        return false;
    }
    QJsonObject root;
    jcut::jsonio::readJsonFile(path, &root, nullptr);
    root = runtimeTuningToJson(
        tuning,
        detector,
        tuning.scrfdTargetSize > 0 ? tuning.scrfdTargetSize : scrfdTargetSize);
    if (previewSettings) {
        root.insert(QStringLiteral("preview_debug"), previewDebugSettingsToJson(*previewSettings));
    }
    if (!jcut::jsonio::writeJsonFile(
            path,
            root,
            true,
            errorMessage)) {
        if (errorMessage && errorMessage->isEmpty()) {
            *errorMessage = QStringLiteral("failed to write detector settings: %1").arg(path);
        }
        return false;
    }
    return true;
}

QVector<Detection> sanitizeDetections(const QVector<Detection>& raw,
                                      const QSize& frameSize,
                                      int maxFacesPerFrame,
                                      const RuntimeTuning& tuning,
                                      DetectionSanitizeStats* stats = nullptr)
{
    QVector<Detection> out;
    if (stats) {
        *stats = DetectionSanitizeStats{};
        stats->rawCount = raw.size();
    }
    if (!frameSize.isValid()) {
        if (stats) {
            stats->keptBeforeNms = raw.size();
            stats->keptAfterNms = raw.size();
        }
        return raw;
    }
    out.reserve(raw.size());
    const QRectF roiRect(tuning.roiX1 * frameSize.width(),
                         tuning.roiY1 * frameSize.height(),
                         (tuning.roiX2 - tuning.roiX1) * frameSize.width(),
                         (tuning.roiY2 - tuning.roiY1) * frameSize.height());
    const double frameArea = static_cast<double>(frameSize.width()) * static_cast<double>(frameSize.height());
    for (const auto& d : raw) {
        if (!std::isfinite(d.confidence) || d.confidence < 0.0f || d.confidence > 1.0f) {
            if (stats) {
                ++stats->rejectedInvalidConfidence;
            }
            continue;
        }
        QRectF box = d.box.intersected(QRectF(0, 0, frameSize.width(), frameSize.height()));
        if (box.width() <= 1.0 || box.height() <= 1.0) {
            if (stats) {
                ++stats->rejectedDegenerate;
            }
            continue;
        }
        const QPointF c = box.center();
        if (!roiRect.contains(c)) {
            if (stats) {
                ++stats->rejectedRoi;
            }
            continue;
        }
        const double areaRatio = (box.width() * box.height()) / qMax(1.0, frameArea);
        if (areaRatio < tuning.minFaceAreaRatio || areaRatio > tuning.maxFaceAreaRatio) {
            if (stats) {
                ++stats->rejectedArea;
            }
            continue;
        }
        const double aspect = box.width() / qMax(1.0, box.height());
        if (aspect < tuning.minAspect || aspect > tuning.maxAspect) {
            if (stats) {
                ++stats->rejectedAspect;
            }
            continue;
        }
        out.push_back({box, d.confidence});
    }
    if (stats) {
        stats->keptBeforeNms = out.size();
    }
    std::sort(out.begin(), out.end(), [](const Detection& a, const Detection& b) {
        return a.confidence > b.confidence;
    });
    QVector<Detection> suppressed;
    suppressed.reserve(out.size());
    for (const Detection& candidate : out) {
        bool keep = true;
        for (const Detection& accepted : suppressed) {
            if (jcut::facestream::continuityIou(candidate.box, accepted.box) >
                tuning.nmsIouThreshold) {
                if (stats) {
                    ++stats->rejectedNms;
                }
                keep = false;
                break;
            }
        }
        if (keep) {
            suppressed.push_back(candidate);
        }
    }
    out = suppressed;
    if (stats) {
        stats->keptAfterNms = out.size();
    }
    if (tuning.primaryFaceOnly && out.size() > 1) {
        out.resize(1);
    }
    if (maxFacesPerFrame > 0 && out.size() > maxFacesPerFrame) {
        out.resize(maxFacesPerFrame);
    }
    return out;
}

void logDetectionSanitizeStats(int frameNumber,
                               const QSize& frameSize,
                               const RuntimeTuning& tuning,
                               const DetectionSanitizeStats& stats,
                               const QVector<Detection>& raw)
{
    std::cerr << "facestream_debug"
              << " frame=" << frameNumber
              << " size=" << frameSize.width() << "x" << frameSize.height()
              << " raw=" << stats.rawCount
              << " kept_pre_nms=" << stats.keptBeforeNms
              << " kept_post_nms=" << stats.keptAfterNms
              << " reject_conf=" << stats.rejectedInvalidConfidence
              << " reject_degenerate=" << stats.rejectedDegenerate
              << " reject_roi=" << stats.rejectedRoi
              << " reject_area=" << stats.rejectedArea
              << " reject_aspect=" << stats.rejectedAspect
              << " reject_nms=" << stats.rejectedNms
              << " roi=[" << tuning.roiX1 << "," << tuning.roiY1 << "," << tuning.roiX2 << "," << tuning.roiY2 << "]"
              << " area=[" << tuning.minFaceAreaRatio << "," << tuning.maxFaceAreaRatio << "]"
              << " aspect=[" << tuning.minAspect << "," << tuning.maxAspect << "]"
              << "\n";
    const int sampleCount = std::min(static_cast<int>(raw.size()), 3);
    for (int i = 0; i < sampleCount; ++i) {
        const Detection& det = raw.at(i);
        std::cerr << "facestream_debug_box"
                  << " frame=" << frameNumber
                  << " index=" << i
                  << " conf=" << det.confidence
                  << " box=[" << det.box.x() << "," << det.box.y()
                  << "," << det.box.width() << "," << det.box.height() << "]"
                  << "\n";
    }
}

QRectF flipBoxVertically(const QRectF& box, int imageHeight)
{
    const qreal height = box.height();
    const qreal flippedTop = static_cast<qreal>(imageHeight) - box.y() - height;
    return QRectF(box.x(), flippedTop, box.width(), height);
}

QVector<Detection> flipDetectionsVertically(const QVector<Detection>& raw,
                                            int imageWidth,
                                            int imageHeight)
{
    QVector<Detection> out;
    out.reserve(raw.size());
    const QRectF bounds(0.0, 0.0, imageWidth, imageHeight);
    for (const Detection& det : raw) {
        Detection flipped = det;
        flipped.box = flipBoxVertically(det.box, imageHeight).intersected(bounds);
        out.push_back(flipped);
    }
    return out;
}

void usage(const char* argv0)
{
    std::cerr << "Usage: " << argv0
              << " [video] [--out-dir DIR] [--max-frames N] [--stride N]"
              << " [--start-frame N]"
              << " [--threshold F] [--preview-frames N] [--preview-stride N]"
              << " [--full-video] [--max-detections N] [--max-faces-per-frame N]"
              << " [--primary-face-only] [--multi-face]"
              << " [--small-face-fallback] [--no-small-face-fallback]"
              << " [--nms-iou F] [--track-match-iou F] [--new-track-min-confidence F]"
              << " [--params-file PATH]"
              << " [--clip-json PATH] [--apply-clip-grading]"
              << " [--detector jcut-dnn|scrfd-ncnn-vulkan|jcut-heuristic-zero-copy]"
              << " [--res10-param PATH] [--res10-bin PATH] [--scrfd-param PATH] [--scrfd-bin PATH]"
              << " [--scrfd-model 500m|1g|2.5g|10g|34g]"
              << " [--scrfd-target-size N] [--scrfd-tiling] [--no-scrfd-tiling]"
              << " [--preview-window] [--no-preview-window] [--preview-files]"
              << " [--preflight]"
              << " [--materialized-generate-facestream]"
              << " [--require-zero-copy]"
              << " [--require-hardware-vulkan-frame-path]"
              << " [--allow-cpu-upload-fallback]"
              << " [--progress] [--no-progress]"
              << " [--verbose] [--log-interval N]"
              << " [--decode software|hardware|auto|hardware_zero_copy]\n";
}

bool parseArgs(int argc, char** argv, Options* options)
{
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << name << "\n";
                return nullptr;
            }
            return argv[++i];
        };
        if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            return false;
        }
        if (arg == "--out-dir") {
            const char* v = next("--out-dir");
            if (!v) return false;
            options->outputDir = QString::fromLocal8Bit(v);
        } else if (arg == "--max-frames") {
            const char* v = next("--max-frames");
            if (!v) return false;
            options->maxFrames = std::max(0, std::atoi(v));
        } else if (arg == "--start-frame") {
            const char* v = next("--start-frame");
            if (!v) return false;
            options->startFrame = std::max(0, std::atoi(v));
        } else if (arg == "--stride") {
            const char* v = next("--stride");
            if (!v) return false;
            options->stride = std::max(1, std::atoi(v));
        } else if (arg == "--full-video") {
            options->maxFrames = 0;
        } else if (arg == "--max-detections") {
            const char* v = next("--max-detections");
            if (!v) return false;
            options->maxDetections = std::max(1, std::atoi(v));
        } else if (arg == "--max-faces-per-frame") {
            const char* v = next("--max-faces-per-frame");
            if (!v) return false;
            options->maxFacesPerFrame = std::max(0, std::atoi(v));
        } else if (arg == "--scrfd-target-size") {
            const char* v = next("--scrfd-target-size");
            if (!v) return false;
            options->scrfdTargetSize = std::max(320, std::atoi(v));
        } else if (arg == "--scrfd-model") {
            const char* v = next("--scrfd-model");
            if (!v) return false;
            options->scrfdModelVariant =
                jcut::facestream::normalizeScrfdModelVariantId(QString::fromLocal8Bit(v));
        } else if (arg == "--scrfd-tiling") {
            options->scrfdTiled = true;
        } else if (arg == "--no-scrfd-tiling") {
            options->scrfdTiled = false;
        } else if (arg == "--primary-face-only") {
            options->primaryFaceOnly = true;
        } else if (arg == "--multi-face") {
            options->primaryFaceOnly = false;
        } else if (arg == "--small-face-fallback") {
            options->smallFaceFallback = true;
        } else if (arg == "--no-small-face-fallback") {
            options->smallFaceFallback = false;
        } else if (arg == "--nms-iou") {
            const char* v = next("--nms-iou");
            if (!v) return false;
            options->nmsIouThreshold = std::clamp(static_cast<float>(std::atof(v)), 0.0f, 1.0f);
        } else if (arg == "--track-match-iou") {
            const char* v = next("--track-match-iou");
            if (!v) return false;
            options->trackMatchIouThreshold = std::clamp(static_cast<float>(std::atof(v)), 0.0f, 1.0f);
        } else if (arg == "--new-track-min-confidence") {
            const char* v = next("--new-track-min-confidence");
            if (!v) return false;
            options->newTrackMinConfidence = std::clamp(static_cast<float>(std::atof(v)), 0.0f, 1.0f);
        } else if (arg == "--threshold") {
            const char* v = next("--threshold");
            if (!v) return false;
            options->threshold = std::clamp(static_cast<float>(std::atof(v)), 0.0f, 1.0f);
        } else if (arg == "--preview-frames") {
            const char* v = next("--preview-frames");
            if (!v) return false;
            options->previewFrames = std::max(0, std::atoi(v));
        } else if (arg == "--preview-stride") {
            const char* v = next("--preview-stride");
            if (!v) return false;
            options->previewStride = std::max(1, std::atoi(v));
        } else if (arg == "--detector") {
            const char* v = next("--detector");
            if (!v) return false;
            const QString detector = QString::fromLocal8Bit(v).trimmed().toLower();
            if (detector == QStringLiteral("jcut-heuristic-zero-copy") ||
                detector == QStringLiteral("vulkan-zero-copy-heuristic")) {
                options->heuristicZeroCopyDetector = true;
                options->scrfdDetector = false;
                options->detector = QStringLiteral("jcut-heuristic-zero-copy");
            } else if (detector == QStringLiteral("scrfd") ||
                       detector == QStringLiteral("scrfd-ncnn") ||
                       detector == QStringLiteral("scrfd-ncnn-vulkan")) {
                options->heuristicZeroCopyDetector = false;
                options->scrfdDetector = true;
                options->detector = QStringLiteral("scrfd-ncnn-vulkan");
            } else if (detector == QStringLiteral("jcut-dnn") ||
                       detector == QStringLiteral("jcut-dnn-vulkan") ||
                       detector == QStringLiteral("native-jcut-dnn") ||
                       detector == QStringLiteral("native_vulkan_dnn")) {
                options->heuristicZeroCopyDetector = false;
                options->scrfdDetector = true;
                options->detector = QStringLiteral("jcut-dnn");
            } else if (detector == QStringLiteral("res10-vulkan") ||
                       detector == QStringLiteral("res10-ncnn-vulkan") ||
                       detector == QStringLiteral("res10-ssd-ncnn-vulkan")) {
                options->heuristicZeroCopyDetector = false;
                options->scrfdDetector = false;
                options->detector = QStringLiteral("res10-vulkan");
            } else {
                std::cerr << "Unsupported --detector value: " << v
                          << ". Use jcut-dnn, scrfd-ncnn-vulkan, res10-vulkan, or jcut-heuristic-zero-copy.\n";
                return false;
            }
        } else if (arg == "--res10-param") {
            const char* v = next("--res10-param");
            if (!v) return false;
            options->res10ParamPath = QString::fromLocal8Bit(v);
        } else if (arg == "--res10-bin") {
            const char* v = next("--res10-bin");
            if (!v) return false;
            options->res10BinPath = QString::fromLocal8Bit(v);
        } else if (arg == "--scrfd-param") {
            const char* v = next("--scrfd-param");
            if (!v) return false;
            options->scrfdParamPath = QString::fromLocal8Bit(v);
        } else if (arg == "--scrfd-bin") {
            const char* v = next("--scrfd-bin");
            if (!v) return false;
            options->scrfdBinPath = QString::fromLocal8Bit(v);
        } else if (arg == "--params-file") {
            const char* v = next("--params-file");
            if (!v) return false;
            options->paramsFile = QString::fromLocal8Bit(v);
        } else if (arg == "--clip-json") {
            const char* v = next("--clip-json");
            if (!v) return false;
            options->clipJsonPath = QString::fromLocal8Bit(v);
        } else if (arg == "--apply-clip-grading") {
            options->applyClipGrading = true;
        } else if (arg == "--preview-socket") {
            const char* v = next("--preview-socket");
            if (!v) return false;
            options->previewSocket = QString::fromLocal8Bit(v);
        } else if (arg == "--preview-window") {
            options->livePreview = true;
        } else if (arg == "--no-preview-window") {
            options->livePreview = false;
        } else if (arg == "--preflight") {
            options->preflight = true;
        } else if (arg == "--preview-files") {
            options->writePreviewFiles = true;
        } else if (arg == "--no-preview-files") {
            options->writePreviewFiles = false;
        } else if (arg == "--materialized-generate-facestream") {
            options->materializedGenerateFacestream = true;
        } else if (arg == "--require-zero-copy") {
            options->requireZeroCopy = true;
        } else if (arg == "--require-hardware-vulkan-frame-path") {
            options->requireHardwareVulkanFramePath = true;
        } else if (arg == "--allow-cpu-upload-fallback") {
            options->allowCpuUploadFallback = true;
        } else if (arg == "--verbose") {
            options->verbose = true;
            options->progress = false;
        } else if (arg == "--progress") {
            options->progress = true;
        } else if (arg == "--no-progress") {
            options->progress = false;
        } else if (arg == "--quiet") {
            options->verbose = false;
            options->logInterval = 0;
        } else if (arg == "--log-interval") {
            const char* v = next("--log-interval");
            if (!v) return false;
            options->logInterval = std::max(0, std::atoi(v));
        } else if (arg == "--decode") {
            const char* v = next("--decode");
            if (!v) return false;
            editor::DecodePreference preference = editor::DecodePreference::Software;
            if (!editor::parseDecodePreference(QString::fromLocal8Bit(v), &preference)) {
                std::cerr << "Invalid --decode value: " << v << "\n";
                return false;
            }
            options->decodePreference = preference;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "Unknown argument: " << arg << "\n";
            return false;
        } else {
            options->videoPath = QString::fromLocal8Bit(arg.c_str());
        }
    }
    if (options->requireZeroCopy) {
        options->requireHardwareVulkanFramePath = true;
        if (options->allowCpuUploadFallback) {
            std::cerr << "--require-zero-copy conflicts with --allow-cpu-upload-fallback.\n";
            return false;
        }
        if (options->applyClipGrading) {
            std::cerr << "--require-zero-copy cannot be combined with --apply-clip-grading.\n";
            return false;
        }
        if (options->materializedGenerateFacestream) {
            std::cerr << "--require-zero-copy cannot be combined with --materialized-generate-facestream.\n";
            return false;
        }
        if (!options->previewSocket.trimmed().isEmpty()) {
            std::cerr << "--require-zero-copy cannot be combined with --preview-socket because socket previews require QImage readback.\n";
            return false;
        }
        if (options->writePreviewFiles) {
            std::cerr << "--require-zero-copy cannot be combined with preview file output because preview frames require QImage readback.\n";
            return false;
        }
    }
    return true;
}

bool loadClipFromJsonPath(const QString& path, TimelineClip* clipOut, QString* errorOut = nullptr)
{
    if (clipOut) {
        *clipOut = TimelineClip{};
    }
    QJsonObject object;
    if (!jcut::jsonio::readJsonFile(path, &object, errorOut)) {
        return false;
    }
    if (clipOut) {
        *clipOut = editor::clipFromJson(object);
    }
    return true;
}

QString formatDuration(double seconds)
{
    if (!std::isfinite(seconds) || seconds < 0.0) {
        return QStringLiteral("--:--");
    }
    const int total = qMax(0, static_cast<int>(std::round(seconds)));
    const int hours = total / 3600;
    const int minutes = (total % 3600) / 60;
    const int secs = total % 60;
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QChar('0'))
            .arg(secs, 2, 10, QChar('0'));
    }
    return QStringLiteral("%1:%2")
        .arg(minutes, 2, 10, QChar('0'))
        .arg(secs, 2, 10, QChar('0'));
}

struct AdaptiveEtaSample {
    int current = 0;
    int processed = 0;
    double elapsedSec = 0.0;
};

struct AdaptiveEtaEstimate {
    double decodeFps = 0.0;
    double processedFps = 0.0;
    double etaSec = -1.0;
};

class AdaptiveEtaTracker {
public:
    void observe(int current, int processed, double elapsedSec)
    {
        if (elapsedSec <= 0.0 || !std::isfinite(elapsedSec) || current < 0 || processed < 0) {
            return;
        }
        if (!m_samples.empty()) {
            const AdaptiveEtaSample& last = m_samples.back();
            if (current == last.current && processed == last.processed) {
                return;
            }
            if (elapsedSec <= last.elapsedSec) {
                return;
            }
        }

        m_samples.push_back(AdaptiveEtaSample{current, processed, elapsedSec});
        while (m_samples.size() > 2 &&
               (elapsedSec - m_samples.front().elapsedSec) > kRecentWindowSec) {
            m_samples.pop_front();
        }

        const AdaptiveEtaEstimate recent = recentEstimate();
        if (recent.decodeFps > 0.0) {
            m_smoothedDecodeFps = smooth(m_smoothedDecodeFps, recent.decodeFps);
        }
        if (recent.processedFps > 0.0) {
            m_smoothedProcessedFps = smooth(m_smoothedProcessedFps, recent.processedFps);
        }
    }

    AdaptiveEtaEstimate estimate(int totalFrames, int current, int processed, double elapsedSec) const
    {
        AdaptiveEtaEstimate result;
        if (elapsedSec <= 0.0 || !std::isfinite(elapsedSec)) {
            return result;
        }

        const double avgDecodeFps = current > 0 ? static_cast<double>(current) / elapsedSec : 0.0;
        const double avgProcessedFps = processed > 0 ? static_cast<double>(processed) / elapsedSec : 0.0;
        const AdaptiveEtaEstimate recent = recentEstimate();

        result.decodeFps = selectRate(avgDecodeFps, recent.decodeFps, m_smoothedDecodeFps, current);
        result.processedFps = selectRate(avgProcessedFps, recent.processedFps, m_smoothedProcessedFps, processed);
        if (result.decodeFps > 0.0) {
            result.etaSec = static_cast<double>(qMax(0, totalFrames - current)) / result.decodeFps;
        }
        return result;
    }

private:
    static constexpr double kRecentWindowSec = 20.0;
    static constexpr double kMinRecentWindowSec = 2.0;
    static constexpr int kWarmupFrames = 180;

    AdaptiveEtaEstimate recentEstimate() const
    {
        AdaptiveEtaEstimate result;
        if (m_samples.size() < 2) {
            return result;
        }
        const AdaptiveEtaSample& first = m_samples.front();
        const AdaptiveEtaSample& last = m_samples.back();
        const double dt = last.elapsedSec - first.elapsedSec;
        if (dt < kMinRecentWindowSec) {
            return result;
        }
        const int decodedDelta = last.current - first.current;
        const int processedDelta = last.processed - first.processed;
        if (decodedDelta > 0) {
            result.decodeFps = static_cast<double>(decodedDelta) / dt;
        }
        if (processedDelta > 0) {
            result.processedFps = static_cast<double>(processedDelta) / dt;
        }
        return result;
    }

    static double smooth(double prior, double current)
    {
        if (prior <= 0.0) {
            return current;
        }
        constexpr double alpha = 0.25;
        return (alpha * current) + ((1.0 - alpha) * prior);
    }

    static double selectRate(double averageRate, double recentRate, double smoothedRate, int completedUnits)
    {
        const double adaptiveRate = smoothedRate > 0.0
            ? smoothedRate
            : (recentRate > 0.0 ? recentRate : averageRate);
        if (completedUnits <= 0) {
            return adaptiveRate;
        }
        if (averageRate <= 0.0) {
            return adaptiveRate;
        }
        const double warmup = std::clamp(static_cast<double>(completedUnits) /
                                             static_cast<double>(kWarmupFrames),
                                         0.0,
                                         1.0);
        if (adaptiveRate <= 0.0) {
            return averageRate;
        }
        return ((1.0 - warmup) * averageRate) + (warmup * adaptiveRate);
    }

    std::deque<AdaptiveEtaSample> m_samples;
    double m_smoothedDecodeFps = 0.0;
    double m_smoothedProcessedFps = 0.0;
};

void renderProgressLine(int frameOffset,
                        int totalFrames,
                        int frameNumber,
                        int processed,
                        int totalDetections,
                        int currentFrameDetections,
                        double elapsedSec,
                        double avgDetectMs,
                        AdaptiveEtaTracker* etaTracker)
{
    const int current = qBound(0, frameOffset + 1, qMax(1, totalFrames));
    const double ratio = qBound(0.0, static_cast<double>(current) / static_cast<double>(qMax(1, totalFrames)), 1.0);
    constexpr int barWidth = 32;
    const int filled = qBound(0, static_cast<int>(std::round(ratio * barWidth)), barWidth);
    std::string bar;
    bar.reserve(barWidth);
    for (int i = 0; i < barWidth; ++i) {
        bar.push_back(i < filled ? '#' : '-');
    }
    if (etaTracker) {
        etaTracker->observe(current, processed, elapsedSec);
    }
    const AdaptiveEtaEstimate eta = etaTracker
        ? etaTracker->estimate(totalFrames, current, processed, elapsedSec)
        : AdaptiveEtaEstimate{};
    const double decodedFps = eta.decodeFps > 0.0
        ? eta.decodeFps
        : (elapsedSec > 0.0 ? static_cast<double>(current) / elapsedSec : 0.0);
    const double processedFps = eta.processedFps > 0.0
        ? eta.processedFps
        : (elapsedSec > 0.0 ? static_cast<double>(processed) / elapsedSec : 0.0);

    std::cout << '\r'
              << '[' << bar << "] "
              << std::fixed << std::setprecision(1) << (ratio * 100.0) << "% "
              << current << '/' << totalFrames
              << " frame=" << frameNumber
              << " processed=" << processed
              << " frame_det=" << currentFrameDetections
              << " det=" << totalDetections
              << " fps=" << std::setprecision(1) << decodedFps
              << " proc/s=" << std::setprecision(2) << processedFps
              << " infer=" << std::setprecision(2) << avgDetectMs << "ms"
              << " eta=" << formatDuration(eta.etaSec).toStdString()
              << "    " << std::flush;
}

bool shouldRenderProgress(int frameOffset,
                          int totalFrames,
                          int processed,
                          int* lastRenderedPercent,
                          std::chrono::steady_clock::time_point* lastRenderedAt)
{
    if (!lastRenderedPercent || !lastRenderedAt) {
        return true;
    }
    const int current = qBound(0, frameOffset + 1, qMax(1, totalFrames));
    if (current >= totalFrames) {
        return true;
    }
    if (processed <= 1 && *lastRenderedPercent < 0) {
        return true;
    }
    const int percent = static_cast<int>(
        std::floor((100.0 * static_cast<double>(current)) / static_cast<double>(qMax(1, totalFrames))));
    const auto now = std::chrono::steady_clock::now();
    const double elapsedSec = std::chrono::duration<double>(now - *lastRenderedAt).count();
    if (percent >= *lastRenderedPercent + 5 || elapsedSec >= 2.0) {
        *lastRenderedPercent = percent;
        *lastRenderedAt = now;
        return true;
    }
    return false;
}

bool applyRuntimeParamsFile(const QString& path,
                            const QFileInfo& info,
                            RuntimeTuning* tuning,
                            PreviewDebugSettings* previewSettings,
                            QDateTime* lastAppliedMtime)
{
    if (!tuning || !lastAppliedMtime || path.isEmpty()) {
        return false;
    }
    if (!info.exists()) {
        return false;
    }
    const QDateTime mtime = info.lastModified();
    if (lastAppliedMtime->isValid() && mtime <= *lastAppliedMtime) {
        return false;
    }

    QJsonObject o;
    if (!jcut::jsonio::readJsonFile(path, &o)) {
        return false;
    }
    if (o.contains(QStringLiteral("scrfd_model_variant"))) {
        tuning->scrfdModelVariant =
            jcut::facestream::normalizeScrfdModelVariantId(
                o.value(QStringLiteral("scrfd_model_variant")).toString(tuning->scrfdModelVariant));
    } else {
        tuning->scrfdModelVariant =
            jcut::facestream::normalizeScrfdModelVariantId(tuning->scrfdModelVariant);
    }
    if (o.contains(QStringLiteral("stride"))) {
        tuning->stride = qMax(1, o.value(QStringLiteral("stride")).toInt(tuning->stride));
    }
    if (o.contains(QStringLiteral("max_detections"))) {
        tuning->maxDetections = qMax(1, o.value(QStringLiteral("max_detections")).toInt(tuning->maxDetections));
    }
    if (o.contains(QStringLiteral("scrfd_target_size"))) {
        tuning->scrfdTargetSize = qMax(320, o.value(QStringLiteral("scrfd_target_size")).toInt(tuning->scrfdTargetSize));
    }
    if (o.contains(QStringLiteral("max_faces_per_frame"))) {
        tuning->maxFacesPerFrame = qMax(0, o.value(QStringLiteral("max_faces_per_frame")).toInt(tuning->maxFacesPerFrame));
    }
    if (o.contains(QStringLiteral("threshold"))) {
        tuning->threshold = std::clamp(static_cast<float>(o.value(QStringLiteral("threshold")).toDouble(tuning->threshold)),
                                       0.0f,
                                       1.0f);
    }
    if (o.contains(QStringLiteral("nms_iou_threshold"))) {
        tuning->nmsIouThreshold = std::clamp(static_cast<float>(o.value(QStringLiteral("nms_iou_threshold")).toDouble(tuning->nmsIouThreshold)),
                                             0.0f,
                                             1.0f);
    }
    if (o.contains(QStringLiteral("track_match_iou_threshold"))) {
        tuning->trackMatchIouThreshold = std::clamp(static_cast<float>(o.value(QStringLiteral("track_match_iou_threshold")).toDouble(tuning->trackMatchIouThreshold)),
                                                    0.0f,
                                                    1.0f);
    }
    if (o.contains(QStringLiteral("new_track_min_confidence"))) {
        tuning->newTrackMinConfidence = std::clamp(static_cast<float>(o.value(QStringLiteral("new_track_min_confidence")).toDouble(tuning->newTrackMinConfidence)),
                                                   0.0f,
                                                   1.0f);
    }
    if (o.contains(QStringLiteral("primary_face_only"))) {
        tuning->primaryFaceOnly = o.value(QStringLiteral("primary_face_only")).toBool(tuning->primaryFaceOnly);
    }
    if (o.contains(QStringLiteral("small_face_fallback"))) {
        tuning->smallFaceFallback = o.value(QStringLiteral("small_face_fallback")).toBool(tuning->smallFaceFallback);
    }
    if (o.contains(QStringLiteral("scrfd_tiled"))) {
        tuning->scrfdTiled = o.value(QStringLiteral("scrfd_tiled")).toBool(tuning->scrfdTiled);
    }
    if (o.contains(QStringLiteral("roi_x1"))) tuning->roiX1 = std::clamp(static_cast<float>(o.value(QStringLiteral("roi_x1")).toDouble(tuning->roiX1)), 0.0f, 1.0f);
    if (o.contains(QStringLiteral("roi_y1"))) tuning->roiY1 = std::clamp(static_cast<float>(o.value(QStringLiteral("roi_y1")).toDouble(tuning->roiY1)), 0.0f, 1.0f);
    if (o.contains(QStringLiteral("roi_x2"))) tuning->roiX2 = std::clamp(static_cast<float>(o.value(QStringLiteral("roi_x2")).toDouble(tuning->roiX2)), 0.0f, 1.0f);
    if (o.contains(QStringLiteral("roi_y2"))) tuning->roiY2 = std::clamp(static_cast<float>(o.value(QStringLiteral("roi_y2")).toDouble(tuning->roiY2)), 0.0f, 1.0f);
    if (tuning->roiX2 < tuning->roiX1) std::swap(tuning->roiX1, tuning->roiX2);
    if (tuning->roiY2 < tuning->roiY1) std::swap(tuning->roiY1, tuning->roiY2);
    if (o.contains(QStringLiteral("min_face_area_ratio"))) tuning->minFaceAreaRatio = std::clamp(static_cast<float>(o.value(QStringLiteral("min_face_area_ratio")).toDouble(tuning->minFaceAreaRatio)), 0.0f, 1.0f);
    if (o.contains(QStringLiteral("max_face_area_ratio"))) tuning->maxFaceAreaRatio = std::clamp(static_cast<float>(o.value(QStringLiteral("max_face_area_ratio")).toDouble(tuning->maxFaceAreaRatio)), 0.0f, 1.0f);
    if (tuning->maxFaceAreaRatio < tuning->minFaceAreaRatio) std::swap(tuning->minFaceAreaRatio, tuning->maxFaceAreaRatio);
    if (o.contains(QStringLiteral("min_aspect"))) tuning->minAspect = std::clamp(static_cast<float>(o.value(QStringLiteral("min_aspect")).toDouble(tuning->minAspect)), 0.0f, 100.0f);
    if (o.contains(QStringLiteral("max_aspect"))) tuning->maxAspect = std::clamp(static_cast<float>(o.value(QStringLiteral("max_aspect")).toDouble(tuning->maxAspect)), 0.0f, 100.0f);
    if (tuning->maxAspect < tuning->minAspect) std::swap(tuning->minAspect, tuning->maxAspect);
    if (previewSettings) {
        applyPreviewDebugSettingsObject(o.value(QStringLiteral("preview_debug")).toObject(), previewSettings);
    }
    *lastAppliedMtime = mtime;
    return true;
}

struct DetectorControlPanel {
    QWidget* window = nullptr;
    QComboBox* profileCombo = nullptr;
    QComboBox* scrfdModelVariant = nullptr;
    QSlider* stride = nullptr;
    QLabel* strideValue = nullptr;
    QSlider* maxDetections = nullptr;
    QLabel* maxDetectionsValue = nullptr;
    QSlider* scrfdTargetSize = nullptr;
    QLabel* scrfdTargetSizeValue = nullptr;
    QSlider* threshold = nullptr;
    QLabel* thresholdValue = nullptr;
    QSlider* nms = nullptr;
    QLabel* nmsValue = nullptr;
    QSlider* trackMatch = nullptr;
    QLabel* trackMatchValue = nullptr;
    QSlider* newTrack = nullptr;
    QLabel* newTrackValue = nullptr;
    QSlider* roiX1 = nullptr;
    QLabel* roiX1Value = nullptr;
    QSlider* roiY1 = nullptr;
    QLabel* roiY1Value = nullptr;
    QSlider* roiX2 = nullptr;
    QLabel* roiX2Value = nullptr;
    QSlider* roiY2 = nullptr;
    QLabel* roiY2Value = nullptr;
    QSlider* minArea = nullptr;
    QLabel* minAreaValue = nullptr;
    QSlider* maxArea = nullptr;
    QLabel* maxAreaValue = nullptr;
    QSlider* minAspect = nullptr;
    QLabel* minAspectValue = nullptr;
    QSlider* maxAspect = nullptr;
    QLabel* maxAspectValue = nullptr;
    QSlider* maxFaces = nullptr;
    QLabel* maxFacesValue = nullptr;
    QCheckBox* primaryFaceOnly = nullptr;
    QCheckBox* smallFaceFallback = nullptr;
    QCheckBox* scrfdTiled = nullptr;
    QCheckBox* applyClipGrading = nullptr;
    QPushButton* pauseButton = nullptr;
    QPushButton* previewPlayButton = nullptr;
    QCheckBox* previewFollowLatest = nullptr;
    QCheckBox* previewShowDetections = nullptr;
    QCheckBox* previewShowTracks = nullptr;
    QCheckBox* previewShowLabels = nullptr;
    QCheckBox* previewShowConfirmed = nullptr;
    QCheckBox* previewShowTentative = nullptr;
    QCheckBox* previewShowLost = nullptr;
    QSlider* previewSeek = nullptr;
    QLabel* previewSeekValue = nullptr;
    QSlider* previewSpeed = nullptr;
    QLabel* previewSpeedValue = nullptr;
    QSlider* previewDetectionLine = nullptr;
    QLabel* previewDetectionLineValue = nullptr;
    QSlider* previewTrackLine = nullptr;
    QLabel* previewTrackLineValue = nullptr;
    QSlider* previewOpacity = nullptr;
    QLabel* previewOpacityValue = nullptr;
    QProgressBar* runtimeProgress = nullptr;
    QLabel* runtimeFrame = nullptr;
    QLabel* runtimeFps = nullptr;
    QLabel* runtimeProcessedFps = nullptr;
    QLabel* runtimeEta = nullptr;
    QLabel* runtimeElapsed = nullptr;
    QLabel* runtimeDetections = nullptr;
    QLabel* runtimeTracks = nullptr;
    QLabel* settingsPath = nullptr;
    std::shared_ptr<bool> syncing = std::make_shared<bool>(false);
};

QString percentText(float value)
{
    return QStringLiteral("%1").arg(static_cast<double>(value), 0, 'f', 2);
}

QString areaRatioText(float value)
{
    return QStringLiteral("%1%").arg(static_cast<double>(value * 100.0f), 0, 'f', 3);
}

QString aspectText(float value)
{
    return QStringLiteral("%1").arg(static_cast<double>(value), 0, 'f', 2);
}

RuntimeTuning runtimeTuningFromDetectorSettings(const jcut::facestream::DetectorRuntimeSettings& settings)
{
    RuntimeTuning tuning;
    tuning.stride = settings.stride;
    tuning.maxDetections = settings.maxDetections;
    tuning.scrfdTargetSize = settings.scrfdTargetSize;
    tuning.maxFacesPerFrame = settings.maxFacesPerFrame;
    tuning.scrfdModelVariant = jcut::facestream::normalizeScrfdModelVariantId(settings.scrfdModelVariant);
    tuning.threshold = settings.threshold;
    tuning.nmsIouThreshold = settings.nmsIouThreshold;
    tuning.trackMatchIouThreshold = settings.trackMatchIouThreshold;
    tuning.newTrackMinConfidence = settings.newTrackMinConfidence;
    tuning.primaryFaceOnly = settings.primaryFaceOnly;
    tuning.smallFaceFallback = settings.smallFaceFallback;
    tuning.scrfdTiled = settings.scrfdTiled;
    tuning.roiX1 = settings.roiX1;
    tuning.roiY1 = settings.roiY1;
    tuning.roiX2 = settings.roiX2;
    tuning.roiY2 = settings.roiY2;
    tuning.minFaceAreaRatio = settings.minFaceAreaRatio;
    tuning.maxFaceAreaRatio = settings.maxFaceAreaRatio;
    tuning.minAspect = settings.minAspect;
    tuning.maxAspect = settings.maxAspect;
    return tuning;
}

void syncDetectorControlPanel(DetectorControlPanel* panel, const RuntimeTuning& tuning)
{
    if (!panel || !panel->window) {
        return;
    }
    *panel->syncing = true;
    if (panel->scrfdModelVariant) {
        const int index = panel->scrfdModelVariant->findData(
            jcut::facestream::normalizeScrfdModelVariantId(tuning.scrfdModelVariant));
        if (index >= 0) {
            panel->scrfdModelVariant->setCurrentIndex(index);
        }
    }
    if (panel->stride) panel->stride->setValue(tuning.stride);
    if (panel->strideValue) panel->strideValue->setText(QString::number(tuning.stride));
    if (panel->maxDetections) panel->maxDetections->setValue(tuning.maxDetections);
    if (panel->maxDetectionsValue) panel->maxDetectionsValue->setText(QString::number(tuning.maxDetections));
    if (panel->scrfdTargetSize) panel->scrfdTargetSize->setValue(tuning.scrfdTargetSize);
    if (panel->scrfdTargetSizeValue) panel->scrfdTargetSizeValue->setText(QString::number(tuning.scrfdTargetSize));
    if (panel->threshold) panel->threshold->setValue(qRound(tuning.threshold * 100.0f));
    if (panel->thresholdValue) panel->thresholdValue->setText(percentText(tuning.threshold));
    if (panel->nms) panel->nms->setValue(qRound(tuning.nmsIouThreshold * 100.0f));
    if (panel->nmsValue) panel->nmsValue->setText(percentText(tuning.nmsIouThreshold));
    if (panel->trackMatch) panel->trackMatch->setValue(qRound(tuning.trackMatchIouThreshold * 100.0f));
    if (panel->trackMatchValue) panel->trackMatchValue->setText(percentText(tuning.trackMatchIouThreshold));
    if (panel->newTrack) panel->newTrack->setValue(qRound(tuning.newTrackMinConfidence * 100.0f));
    if (panel->newTrackValue) panel->newTrackValue->setText(percentText(tuning.newTrackMinConfidence));
    if (panel->roiX1) panel->roiX1->setValue(qRound(tuning.roiX1 * 100.0f));
    if (panel->roiX1Value) panel->roiX1Value->setText(percentText(tuning.roiX1));
    if (panel->roiY1) panel->roiY1->setValue(qRound(tuning.roiY1 * 100.0f));
    if (panel->roiY1Value) panel->roiY1Value->setText(percentText(tuning.roiY1));
    if (panel->roiX2) panel->roiX2->setValue(qRound(tuning.roiX2 * 100.0f));
    if (panel->roiX2Value) panel->roiX2Value->setText(percentText(tuning.roiX2));
    if (panel->roiY2) panel->roiY2->setValue(qRound(tuning.roiY2 * 100.0f));
    if (panel->roiY2Value) panel->roiY2Value->setText(percentText(tuning.roiY2));
    if (panel->minArea) panel->minArea->setValue(qRound(tuning.minFaceAreaRatio * 100000.0f));
    if (panel->minAreaValue) panel->minAreaValue->setText(areaRatioText(tuning.minFaceAreaRatio));
    if (panel->maxArea) panel->maxArea->setValue(qRound(tuning.maxFaceAreaRatio * 100000.0f));
    if (panel->maxAreaValue) panel->maxAreaValue->setText(areaRatioText(tuning.maxFaceAreaRatio));
    if (panel->minAspect) panel->minAspect->setValue(qRound(tuning.minAspect * 100.0f));
    if (panel->minAspectValue) panel->minAspectValue->setText(aspectText(tuning.minAspect));
    if (panel->maxAspect) panel->maxAspect->setValue(qRound(tuning.maxAspect * 100.0f));
    if (panel->maxAspectValue) panel->maxAspectValue->setText(aspectText(tuning.maxAspect));
    if (panel->maxFaces) panel->maxFaces->setValue(tuning.maxFacesPerFrame);
    if (panel->maxFacesValue) {
        panel->maxFacesValue->setText(tuning.maxFacesPerFrame == 0
                                          ? QStringLiteral("unlimited")
                                          : QString::number(tuning.maxFacesPerFrame));
    }
    if (panel->primaryFaceOnly) panel->primaryFaceOnly->setChecked(tuning.primaryFaceOnly);
    if (panel->smallFaceFallback) panel->smallFaceFallback->setChecked(tuning.smallFaceFallback);
    if (panel->scrfdTiled) panel->scrfdTiled->setChecked(tuning.scrfdTiled);
    *panel->syncing = false;
}

void syncDetectorPreviewPanel(DetectorControlPanel* panel,
                              ImGuiPreviewWindow* previewWindow,
                              int minFrame,
                              int latestProcessedFrame)
{
    if (!panel || !panel->window || !previewWindow) {
        return;
    }
    *panel->syncing = true;
    if (panel->pauseButton) {
        panel->pauseButton->setText(
            previewWindow->processingPausedRequested()
                ? QStringLiteral("Resume")
                : QStringLiteral("Pause"));
    }
    if (panel->previewPlayButton) {
        panel->previewPlayButton->setText(
            previewWindow->previewPlaybackActive()
                ? QStringLiteral("Pause Preview")
                : QStringLiteral("Play Preview"));
    }
    if (panel->previewFollowLatest) {
        panel->previewFollowLatest->setChecked(previewWindow->followLatest());
    }
    if (panel->previewShowDetections) {
        panel->previewShowDetections->setChecked(previewWindow->showDetections());
    }
    if (panel->previewShowTracks) {
        panel->previewShowTracks->setChecked(previewWindow->showTracks());
    }
    if (panel->previewShowLabels) {
        panel->previewShowLabels->setChecked(previewWindow->showLabels());
    }
    if (panel->previewShowConfirmed) {
        panel->previewShowConfirmed->setChecked(previewWindow->showConfirmedTracks());
    }
    if (panel->previewShowTentative) {
        panel->previewShowTentative->setChecked(previewWindow->showTentativeTracks());
    }
    if (panel->previewShowLost) {
        panel->previewShowLost->setChecked(previewWindow->showLostTracks());
    }
    if (panel->previewSeek) {
        panel->previewSeek->setRange(minFrame, qMax(minFrame, latestProcessedFrame));
        panel->previewSeek->setValue(previewWindow->requestedPreviewFrame());
    }
    if (panel->previewSeekValue) {
        panel->previewSeekValue->setText(QString::number(previewWindow->requestedPreviewFrame()));
    }
    if (panel->previewSpeed) {
        panel->previewSpeed->setValue(qRound(previewWindow->previewPlaybackSpeed() * 100.0f));
    }
    if (panel->previewSpeedValue) {
        panel->previewSpeedValue->setText(
            QStringLiteral("%1x").arg(previewWindow->previewPlaybackSpeed(), 0, 'f', 2));
    }
    if (panel->previewDetectionLine) {
        panel->previewDetectionLine->setValue(qRound(previewWindow->detectionLineThickness() * 10.0f));
    }
    if (panel->previewDetectionLineValue) {
        panel->previewDetectionLineValue->setText(
            QStringLiteral("%1").arg(previewWindow->detectionLineThickness(), 0, 'f', 1));
    }
    if (panel->previewTrackLine) {
        panel->previewTrackLine->setValue(qRound(previewWindow->trackLineThickness() * 10.0f));
    }
    if (panel->previewTrackLineValue) {
        panel->previewTrackLineValue->setText(
            QStringLiteral("%1").arg(previewWindow->trackLineThickness(), 0, 'f', 1));
    }
    if (panel->previewOpacity) {
        panel->previewOpacity->setValue(qRound(previewWindow->overlayOpacity() * 100.0f));
    }
    if (panel->previewOpacityValue) {
        panel->previewOpacityValue->setText(
            QStringLiteral("%1").arg(previewWindow->overlayOpacity(), 0, 'f', 2));
    }
    *panel->syncing = false;
}

void updateDetectorRuntimeStats(DetectorControlPanel* panel,
                                int frameOffset,
                                int totalFrames,
                                int frameNumber,
                                int processed,
                                int totalDetections,
                                int currentLiveTracks,
                                double elapsedSec,
                                AdaptiveEtaTracker* etaTracker)
{
    if (!panel || !panel->window || !panel->runtimeProgress) {
        return;
    }
    const int current = qBound(0, frameOffset + 1, qMax(1, totalFrames));
    const int progressPercent = static_cast<int>(
        std::round((100.0 * static_cast<double>(current)) / static_cast<double>(qMax(1, totalFrames))));
    if (etaTracker) {
        etaTracker->observe(current, processed, elapsedSec);
    }
    const AdaptiveEtaEstimate eta = etaTracker
        ? etaTracker->estimate(totalFrames, current, processed, elapsedSec)
        : AdaptiveEtaEstimate{};
    const double decodeFps = eta.decodeFps > 0.0
        ? eta.decodeFps
        : (elapsedSec > 0.0 ? static_cast<double>(current) / elapsedSec : 0.0);
    const double processFps = eta.processedFps > 0.0
        ? eta.processedFps
        : (elapsedSec > 0.0 ? static_cast<double>(processed) / elapsedSec : 0.0);

    panel->runtimeProgress->setValue(qBound(0, progressPercent, 100));
    panel->runtimeFrame->setText(QStringLiteral("%1 / %2 (src frame %3)")
                                     .arg(current)
                                     .arg(totalFrames)
                                     .arg(frameNumber));
    panel->runtimeFps->setText(QStringLiteral("%1").arg(decodeFps, 0, 'f', 2));
    panel->runtimeProcessedFps->setText(QStringLiteral("%1").arg(processFps, 0, 'f', 2));
    panel->runtimeEta->setText(formatDuration(eta.etaSec));
    panel->runtimeElapsed->setText(formatDuration(elapsedSec));
    panel->runtimeDetections->setText(QStringLiteral("%1").arg(totalDetections));
    panel->runtimeTracks->setText(QStringLiteral("%1").arg(currentLiveTracks));
}

DetectorControlPanel createDetectorControlPanel(RuntimeTuning* tuning,
                                                const QString& detector,
                                                int scrfdTargetSize,
                                                const QString& settingsPath,
                                                bool* applyClipGrading,
                                                bool allowApplyClipGrading,
                                                ImGuiPreviewWindow* previewWindow,
                                                bool* paused)
{
    DetectorControlPanel panel;
    panel.window = new QWidget;
    panel.window->setWindowTitle(QStringLiteral("JCut Detector Settings"));
    panel.window->setMinimumWidth(420);
    auto* root = new QVBoxLayout(panel.window);
    auto* form = new QFormLayout;
    root->addLayout(form);
    const std::shared_ptr<bool> syncing = panel.syncing;

    auto persist = [tuning, detector, scrfdTargetSize, settingsPath, previewWindow]() {
        PreviewDebugSettings previewSettings;
        PreviewDebugSettings* previewSettingsPtr = nullptr;
        if (previewWindow) {
            previewSettings.followLatest = previewWindow->followLatest();
            previewSettings.showDetections = previewWindow->showDetections();
            previewSettings.showTracks = previewWindow->showTracks();
            previewSettings.showLabels = previewWindow->showLabels();
            previewSettings.showConfirmedTracks = previewWindow->showConfirmedTracks();
            previewSettings.showTentativeTracks = previewWindow->showTentativeTracks();
            previewSettings.showLostTracks = previewWindow->showLostTracks();
            previewSettings.playbackSpeed = previewWindow->previewPlaybackSpeed();
            previewSettings.detectionLineThickness = previewWindow->detectionLineThickness();
            previewSettings.trackLineThickness = previewWindow->trackLineThickness();
            previewSettings.overlayOpacity = previewWindow->overlayOpacity();
            previewSettingsPtr = &previewSettings;
        }
        QString error;
        if (!saveRuntimeTuningFile(settingsPath,
                                   *tuning,
                                   detector,
                                   tuning && tuning->scrfdTargetSize > 0 ? tuning->scrfdTargetSize : scrfdTargetSize,
                                   previewSettingsPtr,
                                   &error) &&
            !error.isEmpty()) {
            qWarning().noquote() << error;
        }
    };
    auto applyProfile = [tuning, persist, &panel](const jcut::facestream::DetectorRuntimeSettings& profileSettings) {
        if (!tuning) {
            return;
        }
        const QString currentScrfdModelVariant =
            jcut::facestream::normalizeScrfdModelVariantId(tuning->scrfdModelVariant);
        *panel.syncing = true;
        *tuning = runtimeTuningFromDetectorSettings(profileSettings);
        tuning->scrfdModelVariant = currentScrfdModelVariant;
        syncDetectorControlPanel(&panel, *tuning);
        *panel.syncing = false;
        persist();
    };

    auto addSlider = [&](const QString& name,
                         const QString& tooltip,
                         int minValue,
                         int maxValue,
                         int initialValue,
                         QSlider** sliderOut,
                         QLabel** labelOut,
                         const std::function<QString(int)>& labelText,
                         const std::function<void(int)>& applyValue) {
        auto* row = new QWidget;
        row->setToolTip(tooltip);
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        auto* slider = new QSlider(Qt::Horizontal);
        slider->setRange(minValue, maxValue);
        slider->setValue(initialValue);
        slider->setToolTip(tooltip);
        auto* valueLabel = new QLabel(labelText(initialValue));
        valueLabel->setMinimumWidth(72);
        valueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        valueLabel->setToolTip(tooltip);
        layout->addWidget(slider, 1);
        layout->addWidget(valueLabel);
        form->addRow(name, row);
        const std::shared_ptr<bool> syncing = panel.syncing;
        QObject::connect(slider, &QSlider::valueChanged, panel.window, [syncing, tuning, valueLabel, labelText, applyValue, persist](int value) {
            valueLabel->setText(labelText(value));
            if (*syncing || !tuning) {
                return;
            }
            applyValue(value);
            persist();
        });
        *sliderOut = slider;
        *labelOut = valueLabel;
    };

    auto* profileRow = new QWidget;
    auto* profileLayout = new QHBoxLayout(profileRow);
    profileLayout->setContentsMargins(0, 0, 0, 0);
    panel.profileCombo = new QComboBox(profileRow);
    auto* applyProfileButton = new QPushButton(QStringLiteral("Apply"), profileRow);
    profileLayout->addWidget(panel.profileCombo, 1);
    profileLayout->addWidget(applyProfileButton);
    form->addRow(QStringLiteral("Profile"), profileRow);
    panel.profileCombo->setToolTip(QStringLiteral(
        "Built-in immutable detector profiles. Applying one overwrites the live tuning and persists it to the active settings file."));
    applyProfileButton->setToolTip(panel.profileCombo->toolTip());
    const QVector<jcut::facestream::DetectorSettingsProfileDefinition> profiles =
        jcut::facestream::builtInDetectorProfiles();
    for (const jcut::facestream::DetectorSettingsProfileDefinition& profile : profiles) {
        panel.profileCombo->addItem(profile.label, profile.id);
        const int index = panel.profileCombo->count() - 1;
        panel.profileCombo->setItemData(
            index,
            QStringLiteral("%1\nstride %2 | target %3 | threshold %4 | max faces %5")
                .arg(profile.description)
                .arg(profile.settings.stride)
                .arg(profile.settings.scrfdTargetSize)
                .arg(static_cast<double>(profile.settings.threshold), 0, 'f', 2)
                .arg(profile.settings.maxFacesPerFrame),
            Qt::ToolTipRole);
    }
    QObject::connect(applyProfileButton, &QPushButton::clicked, panel.window, [applyProfile, &panel, profiles]() {
        if (!panel.profileCombo) {
            return;
        }
        const QString selectedId = panel.profileCombo->currentData().toString();
        for (const jcut::facestream::DetectorSettingsProfileDefinition& profile : profiles) {
            if (profile.id == selectedId) {
                applyProfile(profile.settings);
                return;
            }
        }
    });

    addSlider(QStringLiteral("Stride"),
              QStringLiteral("Run detection every Nth source frame. Lower values improve recall and preview responsiveness; higher values are faster but skip more frames."),
              1, 120, tuning->stride,
              &panel.stride, &panel.strideValue,
              [](int v) { return QString::number(v); },
              [tuning](int v) { tuning->stride = std::max(1, v); });
    addSlider(QStringLiteral("Max detections"),
              QStringLiteral("Maximum number of raw detector boxes kept before post-filtering. Raise this for panels or crowds; lower it to reduce noisy candidates."),
              1, 1024, tuning->maxDetections,
              &panel.maxDetections, &panel.maxDetectionsValue,
              [](int v) { return QString::number(v); },
              [tuning](int v) { tuning->maxDetections = std::max(1, v); });
    const bool scrfdFamily = detector.contains(QStringLiteral("scrfd"), Qt::CaseInsensitive) ||
                             detector.compare(QStringLiteral("jcut-dnn"), Qt::CaseInsensitive) == 0;
    if (scrfdFamily) {
        panel.scrfdModelVariant = new QComboBox;
        panel.scrfdModelVariant->setToolTip(
            QStringLiteral("SCRFD model family variant. Larger variants improve recall, especially for small or crowded faces, at higher GPU cost. Model changes apply on the next run, not mid-run."));
        const QVector<jcut::facestream::ScrfdModelVariantDefinition> variants =
            jcut::facestream::supportedScrfdModelVariants();
        for (const jcut::facestream::ScrfdModelVariantDefinition& variant : variants) {
            panel.scrfdModelVariant->addItem(variant.label, variant.id);
            const int index = panel.scrfdModelVariant->count() - 1;
            panel.scrfdModelVariant->setItemData(index, variant.description, Qt::ToolTipRole);
        }
        panel.scrfdModelVariant->setEnabled(false);
        form->addRow(QStringLiteral("SCRFD model"), panel.scrfdModelVariant);
        addSlider(QStringLiteral("SCRFD target"),
                  QStringLiteral("Input size for the SCRFD model. Raise this to improve small-face recall at higher GPU cost; lower it for speed."),
                  320, 1280, tuning->scrfdTargetSize,
                  &panel.scrfdTargetSize, &panel.scrfdTargetSizeValue,
                  [](int v) { return QString::number(v); },
                  [tuning](int v) { tuning->scrfdTargetSize = std::max(320, v); });
    }
    addSlider(QStringLiteral("Confidence threshold"),
              QStringLiteral("Minimum detector confidence accepted as a face. Lower this to catch smaller or blurrier faces; raise it to reject false positives such as text, hands, or background shapes."),
              1, 99, qRound(tuning->threshold * 100.0f),
              &panel.threshold, &panel.thresholdValue,
              [](int v) { return percentText(v / 100.0f); },
              [tuning](int v) { tuning->threshold = v / 100.0f; });
    addSlider(QStringLiteral("NMS IoU"),
              QStringLiteral("Overlap threshold for merging duplicate face boxes. Lower values suppress nearby duplicate boxes more aggressively; higher values keep more overlapping detections."),
              0, 100, qRound(tuning->nmsIouThreshold * 100.0f),
              &panel.nms, &panel.nmsValue,
              [](int v) { return percentText(v / 100.0f); },
              [tuning](int v) { tuning->nmsIouThreshold = v / 100.0f; });
    addSlider(QStringLiteral("ROI left"),
              QStringLiteral("Left edge of the allowed face region as a percent of frame width. Raise this to ignore detections on the far left."),
              0, 100, qRound(tuning->roiX1 * 100.0f),
              &panel.roiX1, &panel.roiX1Value,
              [](int v) { return percentText(v / 100.0f); },
              [tuning](int v) {
                  tuning->roiX1 = v / 100.0f;
                  if (tuning->roiX2 < tuning->roiX1) std::swap(tuning->roiX1, tuning->roiX2);
              });
    addSlider(QStringLiteral("ROI top"),
              QStringLiteral("Top edge of the allowed face region as a percent of frame height. Raise this to ignore detections near the top."),
              0, 100, qRound(tuning->roiY1 * 100.0f),
              &panel.roiY1, &panel.roiY1Value,
              [](int v) { return percentText(v / 100.0f); },
              [tuning](int v) {
                  tuning->roiY1 = v / 100.0f;
                  if (tuning->roiY2 < tuning->roiY1) std::swap(tuning->roiY1, tuning->roiY2);
              });
    addSlider(QStringLiteral("ROI right"),
              QStringLiteral("Right edge of the allowed face region as a percent of frame width. Lower this to ignore detections on the far right."),
              0, 100, qRound(tuning->roiX2 * 100.0f),
              &panel.roiX2, &panel.roiX2Value,
              [](int v) { return percentText(v / 100.0f); },
              [tuning](int v) {
                  tuning->roiX2 = v / 100.0f;
                  if (tuning->roiX2 < tuning->roiX1) std::swap(tuning->roiX1, tuning->roiX2);
              });
    addSlider(QStringLiteral("ROI bottom"),
              QStringLiteral("Bottom edge of the allowed face region as a percent of frame height. Lower this to ignore detections near the bottom."),
              0, 100, qRound(tuning->roiY2 * 100.0f),
              &panel.roiY2, &panel.roiY2Value,
              [](int v) { return percentText(v / 100.0f); },
              [tuning](int v) {
                  tuning->roiY2 = v / 100.0f;
                  if (tuning->roiY2 < tuning->roiY1) std::swap(tuning->roiY1, tuning->roiY2);
              });
    addSlider(QStringLiteral("Min face area"),
              QStringLiteral("Smallest accepted face box as a percent of frame area. Lower this for distant/small faces; raise it to reject tiny false positives."),
              0, 2000, qRound(tuning->minFaceAreaRatio * 100000.0f),
              &panel.minArea, &panel.minAreaValue,
              [](int v) { return areaRatioText(v / 100000.0f); },
              [tuning](int v) { tuning->minFaceAreaRatio = v / 100000.0f; });
    addSlider(QStringLiteral("Max face area"),
              QStringLiteral("Largest accepted face box as a percent of frame area. Lower this to reject oversized false positives or partial-frame detections; raise it to allow very close faces."),
              0, 100000, qRound(tuning->maxFaceAreaRatio * 100000.0f),
              &panel.maxArea, &panel.maxAreaValue,
              [](int v) { return areaRatioText(v / 100000.0f); },
              [tuning](int v) { tuning->maxFaceAreaRatio = v / 100000.0f; });
    addSlider(QStringLiteral("Min aspect"),
              QStringLiteral("Smallest allowed width/height ratio for a face box. Raise this to reject tall narrow boxes such as arms or fingers."),
              1, 1000, qRound(tuning->minAspect * 100.0f),
              &panel.minAspect, &panel.minAspectValue,
              [](int v) { return aspectText(v / 100.0f); },
              [tuning](int v) {
                  tuning->minAspect = v / 100.0f;
                  if (tuning->maxAspect < tuning->minAspect) std::swap(tuning->minAspect, tuning->maxAspect);
              });
    addSlider(QStringLiteral("Max aspect"),
              QStringLiteral("Largest allowed width/height ratio for a face box. Lower this to reject wide non-face boxes such as hands or partial torsos."),
              1, 1000, qRound(tuning->maxAspect * 100.0f),
              &panel.maxAspect, &panel.maxAspectValue,
              [](int v) { return aspectText(v / 100.0f); },
              [tuning](int v) {
                  tuning->maxAspect = v / 100.0f;
                  if (tuning->maxAspect < tuning->minAspect) std::swap(tuning->minAspect, tuning->maxAspect);
              });
    addSlider(QStringLiteral("Max faces/frame"),
              QStringLiteral("Maximum accepted faces per processed frame after filtering. 0 means unlimited. Use lower values for single-speaker videos and higher values for panels or crowds."),
              0, 64, tuning->maxFacesPerFrame,
              &panel.maxFaces, &panel.maxFacesValue,
              [](int v) { return v == 0 ? QStringLiteral("unlimited") : QString::number(v); },
              [tuning](int v) { tuning->maxFacesPerFrame = v; });

    panel.smallFaceFallback = new QCheckBox(QStringLiteral("Enable Res10 tiled fallback"));
    panel.smallFaceFallback->setToolTip(QStringLiteral("Runs extra cropped Res10 passes when the zero-copy Res10 detector misses. This can help some small faces but is slower and may produce false positives; SCRFD usually handles small faces better."));
    panel.smallFaceFallback->setChecked(tuning->smallFaceFallback);
    form->addRow(QStringLiteral("Small-face fallback"), panel.smallFaceFallback);
    {
        const std::shared_ptr<bool> syncing = panel.syncing;
        QObject::connect(panel.smallFaceFallback, &QCheckBox::toggled, panel.window, [syncing, tuning, persist](bool checked) {
            if (*syncing || !tuning) {
                return;
            }
            tuning->smallFaceFallback = checked;
            persist();
        });
    }
    if (scrfdFamily) {
        panel.scrfdTiled = new QCheckBox(QStringLiteral("Enable SCRFD 2x2 tiled pass"));
        panel.scrfdTiled->setToolTip(QStringLiteral("Runs SCRFD on the full frame plus overlapping 2x2 tiles to improve recall on smaller panel faces at higher GPU cost."));
        panel.scrfdTiled->setChecked(tuning->scrfdTiled);
        form->addRow(QStringLiteral("SCRFD tiling"), panel.scrfdTiled);
        const std::shared_ptr<bool> syncing = panel.syncing;
        QObject::connect(panel.scrfdTiled, &QCheckBox::toggled, panel.window, [syncing, tuning, persist](bool checked) {
            if (*syncing || !tuning) {
                return;
            }
            tuning->scrfdTiled = checked;
            persist();
        });
    }

    if (allowApplyClipGrading && applyClipGrading) {
        panel.applyClipGrading =
            new QCheckBox(QStringLiteral("Apply clip grading during detection"));
        panel.applyClipGrading->setToolTip(QStringLiteral(
            "Use the clip grading payload from --clip-json for both live preview and detection input."));
        panel.applyClipGrading->setChecked(*applyClipGrading);
        form->addRow(QStringLiteral("Clip grading"), panel.applyClipGrading);
        QObject::connect(panel.applyClipGrading,
                         &QCheckBox::toggled,
                         panel.window,
                         [applyClipGrading](bool checked) {
                             if (!applyClipGrading) {
                                 return;
                             }
                             *applyClipGrading = checked;
                         });
    }

    if (paused) {
        panel.pauseButton = new QPushButton(*paused ? QStringLiteral("Resume")
                                                    : QStringLiteral("Pause"));
        panel.pauseButton->setToolTip(QStringLiteral(
            "Pause or resume FaceStream detection while keeping the runtime controls responsive."));
        root->addWidget(panel.pauseButton);
        QObject::connect(panel.pauseButton,
                         &QPushButton::clicked,
                         panel.window,
                         [paused, button = panel.pauseButton]() {
                             if (!paused || !button) {
                                 return;
                             }
                             *paused = !*paused;
                             button->setText(*paused ? QStringLiteral("Resume")
                                                     : QStringLiteral("Pause"));
                         });
    }

    if (previewWindow) {
        auto* previewTitle = new QLabel(QStringLiteral("Preview transport"));
        QFont previewFont = previewTitle->font();
        previewFont.setBold(true);
        previewTitle->setFont(previewFont);
        root->addWidget(previewTitle);

        auto* previewRow = new QHBoxLayout;
        panel.previewPlayButton = new QPushButton(QStringLiteral("Play Preview"));
        panel.previewFollowLatest = new QCheckBox(QStringLiteral("Follow Latest"));
        previewRow->addWidget(panel.previewPlayButton);
        previewRow->addWidget(panel.previewFollowLatest);
        previewRow->addStretch(1);
        root->addLayout(previewRow);

        auto* seekRow = new QHBoxLayout;
        panel.previewSeek = new QSlider(Qt::Horizontal);
        panel.previewSeek->setRange(0, 0);
        panel.previewSeekValue = new QLabel(QStringLiteral("0"));
        panel.previewSeekValue->setMinimumWidth(72);
        seekRow->addWidget(panel.previewSeek, 1);
        seekRow->addWidget(panel.previewSeekValue);
        root->addLayout(seekRow);

        auto* speedRow = new QHBoxLayout;
        auto* speedLabel = new QLabel(QStringLiteral("Preview speed"));
        panel.previewSpeed = new QSlider(Qt::Horizontal);
        panel.previewSpeed->setRange(25, 400);
        panel.previewSpeedValue = new QLabel(QStringLiteral("1.00x"));
        panel.previewSpeedValue->setMinimumWidth(72);
        speedRow->addWidget(speedLabel);
        speedRow->addWidget(panel.previewSpeed, 1);
        speedRow->addWidget(panel.previewSpeedValue);
        root->addLayout(speedRow);

        auto* overlayRow1 = new QHBoxLayout;
        panel.previewShowDetections = new QCheckBox(QStringLiteral("Detections"));
        panel.previewShowTracks = new QCheckBox(QStringLiteral("Tracks"));
        panel.previewShowLabels = new QCheckBox(QStringLiteral("Labels"));
        overlayRow1->addWidget(panel.previewShowDetections);
        overlayRow1->addWidget(panel.previewShowTracks);
        overlayRow1->addWidget(panel.previewShowLabels);
        overlayRow1->addStretch(1);
        root->addLayout(overlayRow1);

        auto* overlayRow2 = new QHBoxLayout;
        panel.previewShowConfirmed = new QCheckBox(QStringLiteral("Confirmed"));
        panel.previewShowTentative = new QCheckBox(QStringLiteral("Tentative"));
        panel.previewShowLost = new QCheckBox(QStringLiteral("Lost"));
        overlayRow2->addWidget(panel.previewShowConfirmed);
        overlayRow2->addWidget(panel.previewShowTentative);
        overlayRow2->addWidget(panel.previewShowLost);
        overlayRow2->addStretch(1);
        root->addLayout(overlayRow2);

        auto* overlayKnobRow1 = new QHBoxLayout;
        auto* detLineLabel = new QLabel(QStringLiteral("Det line"));
        panel.previewDetectionLine = new QSlider(Qt::Horizontal);
        panel.previewDetectionLine->setRange(10, 40);
        panel.previewDetectionLineValue = new QLabel(QStringLiteral("1.5"));
        panel.previewDetectionLineValue->setMinimumWidth(40);
        overlayKnobRow1->addWidget(detLineLabel);
        overlayKnobRow1->addWidget(panel.previewDetectionLine, 1);
        overlayKnobRow1->addWidget(panel.previewDetectionLineValue);
        root->addLayout(overlayKnobRow1);

        auto* overlayKnobRow2 = new QHBoxLayout;
        auto* trackLineLabel = new QLabel(QStringLiteral("Track line"));
        panel.previewTrackLine = new QSlider(Qt::Horizontal);
        panel.previewTrackLine->setRange(10, 50);
        panel.previewTrackLineValue = new QLabel(QStringLiteral("2.5"));
        panel.previewTrackLineValue->setMinimumWidth(40);
        overlayKnobRow2->addWidget(trackLineLabel);
        overlayKnobRow2->addWidget(panel.previewTrackLine, 1);
        overlayKnobRow2->addWidget(panel.previewTrackLineValue);
        root->addLayout(overlayKnobRow2);

        auto* overlayKnobRow3 = new QHBoxLayout;
        auto* opacityLabel = new QLabel(QStringLiteral("Opacity"));
        panel.previewOpacity = new QSlider(Qt::Horizontal);
        panel.previewOpacity->setRange(20, 100);
        panel.previewOpacityValue = new QLabel(QStringLiteral("1.00"));
        panel.previewOpacityValue->setMinimumWidth(40);
        overlayKnobRow3->addWidget(opacityLabel);
        overlayKnobRow3->addWidget(panel.previewOpacity, 1);
        overlayKnobRow3->addWidget(panel.previewOpacityValue);
        root->addLayout(overlayKnobRow3);

        QObject::connect(panel.previewPlayButton,
                         &QPushButton::clicked,
                         panel.window,
                         [syncing, previewWindow]() {
                             if (!previewWindow || !syncing || *syncing) {
                                 return;
                             }
                             previewWindow->setPreviewPlaybackActive(!previewWindow->previewPlaybackActive());
                         });
        QObject::connect(panel.previewFollowLatest,
                         &QCheckBox::toggled,
                         panel.window,
                         [syncing, previewWindow](bool checked) {
                             if (!previewWindow || !syncing || *syncing) {
                                 return;
                             }
                             previewWindow->setFollowLatest(checked);
                         });
        QObject::connect(panel.previewSeek,
                         &QSlider::valueChanged,
                         panel.window,
                         [syncing, previewWindow](int value) {
                             if (!previewWindow || !syncing || *syncing) {
                                 return;
                             }
                             previewWindow->setRequestedPreviewFrame(value);
                         });
        QObject::connect(panel.previewSpeed,
                         &QSlider::valueChanged,
                         panel.window,
                         [syncing, previewWindow](int value) {
                             if (!previewWindow || !syncing || *syncing) {
                                 return;
                             }
                             previewWindow->setPreviewPlaybackSpeed(static_cast<float>(value) / 100.0f);
                         });
        QObject::connect(panel.previewShowDetections,
                         &QCheckBox::toggled,
                         panel.window,
                         [syncing, previewWindow](bool checked) {
                             if (!previewWindow || !syncing || *syncing) {
                                 return;
                             }
                             previewWindow->setShowDetections(checked);
                         });
        QObject::connect(panel.previewShowTracks,
                         &QCheckBox::toggled,
                         panel.window,
                         [syncing, previewWindow](bool checked) {
                             if (!previewWindow || !syncing || *syncing) {
                                 return;
                             }
                             previewWindow->setShowTracks(checked);
                         });
        QObject::connect(panel.previewShowLabels,
                         &QCheckBox::toggled,
                         panel.window,
                         [syncing, previewWindow](bool checked) {
                             if (!previewWindow || !syncing || *syncing) {
                                 return;
                             }
                             previewWindow->setShowLabels(checked);
                         });
        QObject::connect(panel.previewShowConfirmed,
                         &QCheckBox::toggled,
                         panel.window,
                         [syncing, previewWindow](bool checked) {
                             if (!previewWindow || !syncing || *syncing) {
                                 return;
                             }
                             previewWindow->setShowConfirmedTracks(checked);
                         });
        QObject::connect(panel.previewShowTentative,
                         &QCheckBox::toggled,
                         panel.window,
                         [syncing, previewWindow](bool checked) {
                             if (!previewWindow || !syncing || *syncing) {
                                 return;
                             }
                             previewWindow->setShowTentativeTracks(checked);
                         });
        QObject::connect(panel.previewShowLost,
                         &QCheckBox::toggled,
                         panel.window,
                         [syncing, previewWindow](bool checked) {
                             if (!previewWindow || !syncing || *syncing) {
                                 return;
                             }
                             previewWindow->setShowLostTracks(checked);
                         });
        QObject::connect(panel.previewDetectionLine,
                         &QSlider::valueChanged,
                         panel.window,
                         [syncing, previewWindow](int value) {
                             if (!previewWindow || !syncing || *syncing) {
                                 return;
                             }
                             previewWindow->setDetectionLineThickness(static_cast<float>(value) / 10.0f);
                         });
        QObject::connect(panel.previewTrackLine,
                         &QSlider::valueChanged,
                         panel.window,
                         [syncing, previewWindow](int value) {
                             if (!previewWindow || !syncing || *syncing) {
                                 return;
                             }
                             previewWindow->setTrackLineThickness(static_cast<float>(value) / 10.0f);
                         });
        QObject::connect(panel.previewOpacity,
                         &QSlider::valueChanged,
                         panel.window,
                         [syncing, previewWindow](int value) {
                             if (!previewWindow || !syncing || *syncing) {
                                 return;
                             }
                             previewWindow->setOverlayOpacity(static_cast<float>(value) / 100.0f);
                         });
    }

    auto* statsTitle = new QLabel(QStringLiteral("Runtime stats"));
    QFont statsFont = statsTitle->font();
    statsFont.setBold(true);
    statsTitle->setFont(statsFont);
    root->addWidget(statsTitle);

    panel.runtimeProgress = new QProgressBar;
    panel.runtimeProgress->setRange(0, 100);
    panel.runtimeProgress->setValue(0);
    panel.runtimeProgress->setFormat(QStringLiteral("%p%"));
    root->addWidget(panel.runtimeProgress);

    auto* statsForm = new QFormLayout;
    panel.runtimeFrame = new QLabel(QStringLiteral("0 / 0"));
    panel.runtimeFps = new QLabel(QStringLiteral("0.00"));
    panel.runtimeProcessedFps = new QLabel(QStringLiteral("0.00"));
    panel.runtimeEta = new QLabel(QStringLiteral("--:--"));
    panel.runtimeElapsed = new QLabel(QStringLiteral("00:00"));
    panel.runtimeDetections = new QLabel(QStringLiteral("0"));
    panel.runtimeTracks = new QLabel(QStringLiteral("0"));
    statsForm->addRow(QStringLiteral("Frame progress"), panel.runtimeFrame);
    statsForm->addRow(QStringLiteral("Decode FPS"), panel.runtimeFps);
    statsForm->addRow(QStringLiteral("Processed FPS"), panel.runtimeProcessedFps);
    statsForm->addRow(QStringLiteral("ETA"), panel.runtimeEta);
    statsForm->addRow(QStringLiteral("Elapsed"), panel.runtimeElapsed);
    statsForm->addRow(QStringLiteral("Detections"), panel.runtimeDetections);
    statsForm->addRow(QStringLiteral("Live tracks"), panel.runtimeTracks);
    root->addLayout(statsForm);

    panel.settingsPath = new QLabel(settingsPath);
    panel.settingsPath->setToolTip(QStringLiteral("Detector settings are saved here automatically and loaded the next time this video is opened."));
    panel.settingsPath->setTextInteractionFlags(Qt::TextSelectableByMouse);
    panel.settingsPath->setWordWrap(true);
    root->addWidget(panel.settingsPath);

    syncDetectorControlPanel(&panel, *tuning);
    return panel;
}

QString findRes10NcnnModelFile(const QString& explicitPath, const QString& fileName)
{
    if (!explicitPath.isEmpty()) {
        return explicitPath;
    }
    const QStringList roots{
        QDir::currentPath(),
        QCoreApplication::applicationDirPath(),
        QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(QStringLiteral(".."))
    };
    const QStringList rels{
        QStringLiteral("assets/models/%1").arg(fileName),
        QStringLiteral("testbench_assets/models/%1").arg(fileName),
        QStringLiteral("models/%1").arg(fileName)
    };
    for (const QString& root : roots) {
        for (const QString& rel : rels) {
            const QString candidate = QDir(root).absoluteFilePath(rel);
            if (QFileInfo::exists(candidate)) {
                return candidate;
            }
        }
    }
    return QDir::current().absoluteFilePath(QStringLiteral("assets/models/%1").arg(fileName));
}

QString scrfdModelFileName(const QString& variantId, const QString& suffix)
{
    jcut::facestream::ScrfdModelVariantDefinition definition;
    if (jcut::facestream::scrfdModelVariantById(variantId, &definition)) {
        return definition.modelStem + suffix;
    }
    return QStringLiteral("scrfd_500m-opt2") + suffix;
}

uint32_t findMemoryType(VkPhysicalDevice physicalDevice,
                        uint32_t typeBits,
                        VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return UINT32_MAX;
}

class VulkanHarnessContext {
public:
    ~VulkanHarnessContext() { release(); }

    bool initialize(QString* error)
    {
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "jcut-dnn-facestream-generator";
        app.apiVersion = VK_API_VERSION_1_1;

        VkInstanceCreateInfo instanceInfo{};
        instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instanceInfo.pApplicationInfo = &app;
        if (vkCreateInstance(&instanceInfo, nullptr, &instance) != VK_SUCCESS) {
            if (error) *error = QStringLiteral("Failed to create Vulkan instance.");
            return false;
        }

        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
        if (deviceCount == 0) {
            if (error) *error = QStringLiteral("No Vulkan physical devices found.");
            return false;
        }
        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());
        physicalDevice = devices.front();

        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, families.data());
        for (uint32_t i = 0; i < queueFamilyCount; ++i) {
            if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                queueFamilyIndex = i;
                break;
            }
        }
        if (queueFamilyIndex == UINT32_MAX) {
            if (error) *error = QStringLiteral("No Vulkan compute queue found.");
            return false;
        }

        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{};
        queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfo.queueFamilyIndex = queueFamilyIndex;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;

        uint32_t extensionCount = 0;
        vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, nullptr);
        std::vector<VkExtensionProperties> extensionProperties(extensionCount);
        if (extensionCount) {
            vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, extensionProperties.data());
        }
        auto hasDeviceExtension = [&](const char* name) {
            return std::any_of(extensionProperties.begin(), extensionProperties.end(), [&](const VkExtensionProperties& ext) {
                return std::strcmp(ext.extensionName, name) == 0;
            });
        };
        std::vector<const char*> enabledExtensions;
        auto enableIfAvailable = [&](const char* name) {
            if (hasDeviceExtension(name)) {
                enabledExtensions.push_back(name);
            }
        };
        enableIfAvailable(VK_KHR_BIND_MEMORY_2_EXTENSION_NAME);
        enableIfAvailable(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
#ifdef Q_OS_LINUX
        enableIfAvailable(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
#endif
        enableIfAvailable(VK_KHR_DESCRIPTOR_UPDATE_TEMPLATE_EXTENSION_NAME);
        enableIfAvailable(VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME);
        enableIfAvailable(VK_KHR_MAINTENANCE1_EXTENSION_NAME);
        enableIfAvailable(VK_KHR_MAINTENANCE3_EXTENSION_NAME);

        VkDeviceCreateInfo deviceInfo{};
        deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
        deviceInfo.ppEnabledExtensionNames = enabledExtensions.empty() ? nullptr : enabledExtensions.data();
        if (vkCreateDevice(physicalDevice, &deviceInfo, nullptr, &device) != VK_SUCCESS) {
            if (error) *error = QStringLiteral("Failed to create Vulkan device.");
            return false;
        }
        vkGetDeviceQueue(device, queueFamilyIndex, 0, &queue);

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = queueFamilyIndex;
        if (vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool) != VK_SUCCESS) {
            if (error) *error = QStringLiteral("Failed to create command pool.");
            return false;
        }

        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer) != VK_SUCCESS) {
            if (error) *error = QStringLiteral("Failed to allocate command buffer.");
            return false;
        }

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vkCreateFence(device, &fenceInfo, nullptr, &fence) != VK_SUCCESS) {
            if (error) *error = QStringLiteral("Failed to create fence.");
            return false;
        }
        return true;
    }

    bool attachExternalDevice(const jcut::vulkan_detector::VulkanDeviceContext& context, QString* error)
    {
        if (context.physicalDevice == VK_NULL_HANDLE ||
            context.device == VK_NULL_HANDLE ||
            context.queue == VK_NULL_HANDLE ||
            context.queueFamilyIndex == UINT32_MAX) {
            if (error) *error = QStringLiteral("Invalid external Vulkan detector context.");
            return false;
        }
        if (physicalDevice == context.physicalDevice &&
            device == context.device &&
            queue == context.queue &&
            queueFamilyIndex == context.queueFamilyIndex) {
            return true;
        }
        release();
        ownsInstanceAndDevice = false;
        physicalDevice = context.physicalDevice;
        device = context.device;
        queue = context.queue;
        queueFamilyIndex = context.queueFamilyIndex;
        return true;
    }

    void release()
    {
        if (device) {
            vkDeviceWaitIdle(device);
            if (fence) vkDestroyFence(device, fence, nullptr);
            if (commandPool) vkDestroyCommandPool(device, commandPool, nullptr);
            if (imageView) vkDestroyImageView(device, imageView, nullptr);
            if (image) vkDestroyImage(device, image, nullptr);
            if (imageMemory) vkFreeMemory(device, imageMemory, nullptr);
            destroyBuffer(stagingBuffer, stagingMemory);
            destroyBuffer(tensorBuffer, tensorMemory);
            destroyBuffer(detectionBuffer, detectionMemory);
            if (ownsInstanceAndDevice) {
                vkDestroyDevice(device, nullptr);
            }
        }
        if (ownsInstanceAndDevice && instance) {
            vkDestroyInstance(instance, nullptr);
        }
        instance = VK_NULL_HANDLE;
        physicalDevice = VK_NULL_HANDLE;
        device = VK_NULL_HANDLE;
        queue = VK_NULL_HANDLE;
        ownsInstanceAndDevice = true;
    }

    bool ensureDetectorBuffers(VkDeviceSize tensorBytesIn, int maxDetectionsIn, QString* error)
    {
        tensorBytes = tensorBytesIn;
        maxDetections = maxDetectionsIn;
        if (tensorBytes > tensorSize &&
            !createBuffer(tensorBytes,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                          &tensorBuffer,
                          &tensorMemory,
                          &tensorSize,
                          error)) {
            return false;
        }
        const VkDeviceSize detectionBytes = static_cast<VkDeviceSize>(16 + maxDetections * 32);
        if (detectionBytes > detectionSize &&
            !createBuffer(detectionBytes,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          &detectionBuffer,
                          &detectionMemory,
                          &detectionSize,
                          error)) {
            return false;
        }
        return true;
    }

    bool ensureResources(const QSize& size, VkDeviceSize tensorBytes, int maxDetections, QString* error)
    {
        const VkDeviceSize stagingBytes = static_cast<VkDeviceSize>(size.width()) * size.height() * 4;
        if (stagingBytes > stagingSize &&
            !createBuffer(stagingBytes,
                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          &stagingBuffer,
                          &stagingMemory,
                          &stagingSize,
                          error)) {
            return false;
        }
        if (tensorBytes > tensorSize &&
            !createBuffer(tensorBytes,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                          &tensorBuffer,
                          &tensorMemory,
                          &tensorSize,
                          error)) {
            return false;
        }
        const VkDeviceSize detectionBytes = static_cast<VkDeviceSize>(16 + maxDetections * 32);
        if (detectionBytes > detectionSize &&
            !createBuffer(detectionBytes,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          &detectionBuffer,
                          &detectionMemory,
                          &detectionSize,
                          error)) {
            return false;
        }
        if (image && imageSize == size) {
            return true;
        }

        if (imageView) vkDestroyImageView(device, imageView, nullptr);
        if (image) vkDestroyImage(device, image, nullptr);
        if (imageMemory) vkFreeMemory(device, imageMemory, nullptr);
        imageView = VK_NULL_HANDLE;
        image = VK_NULL_HANDLE;
        imageMemory = VK_NULL_HANDLE;
        imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageSize = size;

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent = {static_cast<uint32_t>(size.width()), static_cast<uint32_t>(size.height()), 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateImage(device, &imageInfo, nullptr, &image) != VK_SUCCESS) {
            if (error) *error = QStringLiteral("Failed to create Vulkan image.");
            return false;
        }
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(device, image, &req);
        const uint32_t type = findMemoryType(physicalDevice, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type == UINT32_MAX) {
            if (error) *error = QStringLiteral("No Vulkan image memory type.");
            return false;
        }
        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = type;
        if (vkAllocateMemory(device, &alloc, nullptr, &imageMemory) != VK_SUCCESS) {
            if (error) *error = QStringLiteral("Failed to allocate Vulkan image memory.");
            return false;
        }
        vkBindImageMemory(device, image, imageMemory, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        if (vkCreateImageView(device, &viewInfo, nullptr, &imageView) != VK_SUCCESS) {
            if (error) *error = QStringLiteral("Failed to create Vulkan image view.");
            return false;
        }
        return true;
    }

    bool uploadCpuImage(const QImage& imageIn, double* uploadMs, QString* error)
    {
        const QImage rgba = imageIn.convertToFormat(QImage::Format_RGBA8888);
        if (!ensureResources(rgba.size(), tensorBytes, maxDetections, error)) {
            return false;
        }
        QElapsedTimer timer;
        timer.start();
        void* mapped = nullptr;
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(rgba.width()) * rgba.height() * 4;
        if (vkMapMemory(device, stagingMemory, 0, bytes, 0, &mapped) != VK_SUCCESS) {
            if (error) *error = QStringLiteral("Failed to map staging memory.");
            return false;
        }
        for (int y = 0; y < rgba.height(); ++y) {
            std::memcpy(static_cast<unsigned char*>(mapped) + y * rgba.width() * 4,
                        rgba.constScanLine(y),
                        static_cast<size_t>(rgba.width() * 4));
        }
        vkUnmapMemory(device, stagingMemory);

        vkResetFences(device, 1, &fence);
        vkResetCommandBuffer(commandBuffer, 0);
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(commandBuffer, &begin);
        transition(imageLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = {static_cast<uint32_t>(rgba.width()), static_cast<uint32_t>(rgba.height()), 1};
        vkCmdCopyBufferToImage(commandBuffer, stagingBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        transition(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        vkEndCommandBuffer(commandBuffer);
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commandBuffer;
        vkQueueSubmit(queue, 1, &submit, fence);
        vkWaitForFences(device, 1, &fence, VK_TRUE, 5'000'000'000ull);
        imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        if (uploadMs) *uploadMs = static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
        return true;
    }

    jcut::vulkan_detector::VulkanDeviceContext detectorContext() const
    {
        return {physicalDevice, device, queue, queueFamilyIndex};
    }

    bool createBuffer(VkDeviceSize bytes,
                      VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags properties,
                      VkBuffer* buffer,
                      VkDeviceMemory* memory,
                      VkDeviceSize* storedSize,
                      QString* error)
    {
        destroyBuffer(*buffer, *memory);
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = bytes;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device, &info, nullptr, buffer) != VK_SUCCESS) {
            if (error) *error = QStringLiteral("Failed to create Vulkan buffer.");
            return false;
        }
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device, *buffer, &req);
        const uint32_t type = findMemoryType(physicalDevice, req.memoryTypeBits, properties);
        if (type == UINT32_MAX) {
            if (error) *error = QStringLiteral("No Vulkan buffer memory type.");
            return false;
        }
        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = type;
        if (vkAllocateMemory(device, &alloc, nullptr, memory) != VK_SUCCESS) {
            if (error) *error = QStringLiteral("Failed to allocate Vulkan buffer memory.");
            return false;
        }
        vkBindBufferMemory(device, *buffer, *memory, 0);
        *storedSize = bytes;
        return true;
    }

    void destroyBuffer(VkBuffer& buffer, VkDeviceMemory& memory)
    {
        if (buffer) vkDestroyBuffer(device, buffer, nullptr);
        if (memory) vkFreeMemory(device, memory, nullptr);
        buffer = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
    }

    void transition(VkImageLayout oldLayout, VkImageLayout newLayout)
    {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
        VkPipelineStageFlags src = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkPipelineStageFlags dst = VK_PIPELINE_STAGE_TRANSFER_BIT;
        if (oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            src = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        }
        if (newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        } else if (newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            src = VK_PIPELINE_STAGE_TRANSFER_BIT;
            dst = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        }
        vkCmdPipelineBarrier(commandBuffer, src, dst, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    VkInstance instance = VK_NULL_HANDLE;
    bool ownsInstanceAndDevice = true;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamilyIndex = UINT32_MAX;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory imageMemory = VK_NULL_HANDLE;
    VkImageView imageView = VK_NULL_HANDLE;
    VkImageLayout imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    QSize imageSize;
    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    VkDeviceSize stagingSize = 0;
    VkBuffer tensorBuffer = VK_NULL_HANDLE;
    VkDeviceMemory tensorMemory = VK_NULL_HANDLE;
    VkDeviceSize tensorSize = 0;
    VkDeviceSize tensorBytes = 0;
    VkBuffer detectionBuffer = VK_NULL_HANDLE;
    VkDeviceMemory detectionMemory = VK_NULL_HANDLE;
    VkDeviceSize detectionSize = 0;
    int maxDetections = 256;
};

QVector<Detection> readDetections(VulkanHarnessContext& vk, int imageWidth, int imageHeight)
{
    QVector<Detection> out;
    void* mapped = nullptr;
    if (vkMapMemory(vk.device, vk.detectionMemory, 0, vk.detectionSize, 0, &mapped) != VK_SUCCESS) {
        return out;
    }
    const auto* bytes = static_cast<const unsigned char*>(mapped);
    const uint32_t count = qMin<uint32_t>(*reinterpret_cast<const uint32_t*>(bytes),
                                          static_cast<uint32_t>(vk.maxDetections));
    const float* det = reinterpret_cast<const float*>(bytes + 16);
    out.reserve(static_cast<int>(count));
    for (uint32_t i = 0; i < count; ++i) {
        const float x = det[i * 8 + 0];
        const float y = det[i * 8 + 1];
        const float w = det[i * 8 + 2];
        const float h = det[i * 8 + 3];
        const float c = det[i * 8 + 4];
        QRectF box(x * imageWidth, y * imageHeight, w * imageWidth, h * imageHeight);
        box = box.intersected(QRectF(0, 0, imageWidth, imageHeight));
        if (box.width() >= 8.0 && box.height() >= 8.0) {
            out.push_back({box, c});
        }
    }
    vkUnmapMemory(vk.device, vk.detectionMemory);
    std::sort(out.begin(), out.end(), [](const Detection& a, const Detection& b) {
        return a.confidence > b.confidence;
    });
    return out;
}

QVector<Detection> detectVulkanFrame(jcut::vulkan_detector::VulkanZeroCopyFaceDetector* detector,
                                     VulkanHarnessContext* vk,
                                     const render_detail::OffscreenVulkanFrame& frame,
                                     int maxDetections,
                                     float threshold,
                                     double* vulkanMs,
                                     QString* error)
{
    QVector<Detection> out;
    if (!detector || !vk || !frame.valid) {
        if (error) *error = QStringLiteral("Invalid Vulkan detector frame.");
        return out;
    }
    if (!vk->attachExternalDevice({frame.physicalDevice,
                                   frame.device,
                                   frame.queue,
                                   frame.queueFamilyIndex}, error)) {
        return out;
    }
    if (!detector->isInitialized() &&
        !detector->initialize(vk->detectorContext(), error)) {
        return out;
    }
    if (!vk->ensureDetectorBuffers(detector->tensorSpec().byteSize(), maxDetections, error)) {
        return out;
    }

    QElapsedTimer timer;
    timer.start();
    const jcut::vulkan_detector::VulkanExternalImage source{
        frame.imageView,
        frame.imageLayout,
        frame.size,
        false
    };
    const jcut::vulkan_detector::VulkanTensorBuffer tensor{
        vk->tensorBuffer,
        detector->tensorSpec().byteSize()
    };
    if (!detector->preprocessToTensor(source, tensor, error)) {
        return out;
    }
    const jcut::vulkan_detector::VulkanTensorBuffer detectionBuffer{
        vk->detectionBuffer,
        vk->detectionSize
    };
    if (!detector->inferFromTensor(tensor, detectionBuffer, maxDetections, threshold, error)) {
        return out;
    }
    out = readDetections(*vk, frame.size.width(), frame.size.height());
    if (vulkanMs) {
        *vulkanMs = static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
    }
    return out;
}

QVector<Detection> detectRes10VulkanFrame(jcut::vulkan_detector::VulkanZeroCopyFaceDetector* preprocessor,
                                          jcut::vulkan_detector::VulkanRes10NcnnFaceDetector* detector,
                                          VulkanHarnessContext* vk,
                                          const render_detail::OffscreenVulkanFrame& frame,
                                          const QString& paramPath,
                                          const QString& binPath,
                                          float threshold,
                                          bool smallFaceFallback,
                                          bool suppressNcnnInfo,
                                          double* vulkanMs,
                                          QString* error)
{
    QVector<Detection> out;
    if (!preprocessor || !detector || !vk || !frame.valid) {
        if (error) *error = QStringLiteral("Invalid Vulkan Res10 detector frame.");
        return out;
    }
    if (!vk->attachExternalDevice({frame.physicalDevice,
                                   frame.device,
                                   frame.queue,
                                   frame.queueFamilyIndex}, error)) {
        return out;
    }
    if (!preprocessor->isInitialized() &&
        !preprocessor->initialize(vk->detectorContext(), error)) {
        return out;
    }
    if (!detector->isInitialized()) {
        ScopedStderrSilencer silence(suppressNcnnInfo);
        if (!detector->initialize(vk->detectorContext(), paramPath, binPath, error)) {
            return out;
        }
    }
    if (!vk->ensureDetectorBuffers(preprocessor->tensorSpec().byteSize(), 1, error)) {
        return out;
    }

    QElapsedTimer timer;
    timer.start();
    const jcut::vulkan_detector::VulkanTensorBuffer tensor{
        vk->tensorBuffer,
        preprocessor->tensorSpec().byteSize()
    };
    auto runRoi = [&](const QRectF& roi, QVector<Detection>* dst) -> bool {
        const QRectF bounded = roi.intersected(QRectF(0.0, 0.0, 1.0, 1.0));
        if (bounded.width() <= 0.01 || bounded.height() <= 0.01) {
            return true;
        }
        const jcut::vulkan_detector::VulkanExternalImage source{
            frame.imageView,
            frame.imageLayout,
            frame.size,
            false,
            static_cast<float>(bounded.x()),
            static_cast<float>(bounded.y()),
            static_cast<float>(bounded.width()),
            static_cast<float>(bounded.height())
        };
        if (!preprocessor->preprocessToTensor(source, tensor, error)) {
            return false;
        }
        const int roiWidth = qMax(1, qRound(frame.size.width() * bounded.width()));
        const int roiHeight = qMax(1, qRound(frame.size.height() * bounded.height()));
        const QVector<jcut::vulkan_detector::Res10Detection> raw =
            detector->inferFromTensor(tensor, roiWidth, roiHeight, threshold, error);
        if (!preprocessor->finishPendingPreprocess(error)) {
            return false;
        }
        dst->reserve(dst->size() + raw.size());
        for (const auto& det : raw) {
            QRectF box(bounded.x() * frame.size.width() + det.box.x(),
                       bounded.y() * frame.size.height() + det.box.y(),
                       det.box.width(),
                       det.box.height());
            dst->push_back({box, det.confidence});
        }
        return true;
    };
    if (!runRoi(QRectF(0.0, 0.0, 1.0, 1.0), &out)) {
        return out;
    }
    if (out.isEmpty() && smallFaceFallback) {
        static const QRectF rois[] = {
            QRectF(0.0, 0.0, 0.50, 1.0),
            QRectF(0.25, 0.0, 0.50, 1.0),
            QRectF(0.50, 0.0, 0.50, 1.0),
            QRectF(0.0, 0.0, 0.50, 0.65),
            QRectF(0.25, 0.0, 0.50, 0.65),
            QRectF(0.50, 0.0, 0.50, 0.65)
        };
        for (const QRectF& roi : rois) {
            if (!runRoi(roi, &out)) {
                return out;
            }
        }
    }
    if (vulkanMs) {
        *vulkanMs = static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
    }
    return out;
}

QVector<Detection> detectRes10FromDecoderFrame(jcut::vulkan_detector::VulkanZeroCopyFaceDetector* preprocessor,
                                               jcut::vulkan_detector::VulkanRes10NcnnFaceDetector* detector,
                                               VulkanHarnessContext* vk,
                                               jcut::vulkan_detector::VulkanDetectorFrameHandoff* handoff,
                                               const editor::FrameHandle& frame,
                                               const QString& paramPath,
                                               const QString& binPath,
                                               float threshold,
                                               bool allowCpuUploadFallback,
                                               double* uploadMs,
                                               double* vulkanMs,
                                               bool* hardwareDirectUsed,
                                               QString* error)
{
    QVector<Detection> out;
    if (!preprocessor || !detector || !vk || !handoff || frame.isNull()) {
        if (error) *error = QStringLiteral("Invalid decoder frame for Vulkan detector handoff.");
        return out;
    }
    if (!handoff->uploadFrame(frame, allowCpuUploadFallback, uploadMs, error)) {
        return out;
    }
    if (hardwareDirectUsed) {
        *hardwareDirectUsed =
            handoff->lastMode() == jcut::vulkan_detector::FrameHandoffMode::HardwareDirect;
    }
    const jcut::vulkan_detector::VulkanExternalImage source = handoff->externalImage();
    if (source.imageView == VK_NULL_HANDLE || !source.size.isValid()) {
        if (error) *error = QStringLiteral("Frame handoff produced invalid external image.");
        return out;
    }
    render_detail::OffscreenVulkanFrame detectorFrame;
    detectorFrame.physicalDevice = vk->physicalDevice;
    detectorFrame.device = vk->device;
    detectorFrame.queue = vk->queue;
    detectorFrame.queueFamilyIndex = vk->queueFamilyIndex;
    detectorFrame.imageView = source.imageView;
    detectorFrame.imageLayout = source.imageLayout;
    detectorFrame.size = source.size;
    detectorFrame.queueSupportsCompute = true;
    detectorFrame.valid = true;
    out = detectRes10VulkanFrame(preprocessor,
                                 detector,
                                 vk,
                                 detectorFrame,
                                 paramPath,
                                 binPath,
                                 threshold,
                                 false,
                                 false,
                                 vulkanMs,
                                 error);
    if (!handoff->finishPendingUpload(nullptr, error)) {
        out.clear();
        return out;
    }
    return out;
}

QVector<Detection> detectScrfdVulkanFrame(jcut::vulkan_detector::VulkanZeroCopyFaceDetector* preprocessor,
                                          jcut::vulkan_detector::VulkanScrfdNcnnFaceDetector* detector,
                                          VulkanHarnessContext* vk,
                                          const render_detail::OffscreenVulkanFrame& frame,
                                          const QString& paramPath,
                                          const QString& binPath,
                                          float threshold,
                                          int targetSize,
                                          bool tiledPass,
                                          bool suppressNcnnInfo,
                                          double* vulkanMs,
                                          QString* error)
{
    QVector<Detection> out;
    if (!preprocessor || !detector || !vk || !frame.valid) {
        if (error) *error = QStringLiteral("Invalid Vulkan SCRFD detector frame.");
        return out;
    }
    if (!vk->attachExternalDevice({frame.physicalDevice,
                                   frame.device,
                                   frame.queue,
                                   frame.queueFamilyIndex}, error)) {
        return out;
    }
    if (!preprocessor->isInitialized() &&
        !preprocessor->initialize(vk->detectorContext(), error)) {
        return out;
    }
    if (!detector->isInitialized()) {
        ScopedStderrSilencer silence(suppressNcnnInfo);
        if (!detector->initialize(vk->detectorContext(), paramPath, binPath, error)) {
            return out;
        }
    }

    const int sourceWidth = qMax(1, frame.size.width());
    const int sourceHeight = qMax(1, frame.size.height());
    targetSize = qMax(320, targetSize);
    float scale = 1.0f;
    int resizedW = sourceWidth;
    int resizedH = sourceHeight;
    if (resizedW > resizedH) {
        scale = static_cast<float>(targetSize) / static_cast<float>(resizedW);
        resizedW = targetSize;
        resizedH = qMax(1, qRound(static_cast<float>(resizedH) * scale));
    } else {
        scale = static_cast<float>(targetSize) / static_cast<float>(resizedH);
        resizedH = targetSize;
        resizedW = qMax(1, qRound(static_cast<float>(resizedW) * scale));
    }
    const VkDeviceSize tensorBytes =
        static_cast<VkDeviceSize>(((resizedW + 31) / 32) * 32) *
        static_cast<VkDeviceSize>(((resizedH + 31) / 32) * 32) *
        static_cast<VkDeviceSize>(3 * sizeof(float));
    if (!vk->ensureDetectorBuffers(tensorBytes, 1, error)) {
        return out;
    }

    QElapsedTimer timer;
    timer.start();
    const jcut::vulkan_detector::VulkanTensorBuffer tensor{
        vk->tensorBuffer,
        vk->tensorSize
    };
    auto appendPass = [&](const QRectF& roiNorm) -> bool {
        const QRectF bounded = roiNorm.intersected(QRectF(0.0, 0.0, 1.0, 1.0));
        if (bounded.width() <= 0.05 || bounded.height() <= 0.05) {
            return true;
        }
        const jcut::vulkan_detector::VulkanExternalImage source{
            frame.imageView,
            frame.imageLayout,
            frame.size,
            false,
            static_cast<float>(bounded.x()),
            static_cast<float>(bounded.y()),
            static_cast<float>(bounded.width()),
            static_cast<float>(bounded.height())
        };
        jcut::vulkan_detector::ScrfdTensorLayout layout;
        if (!preprocessor->preprocessScrfdToTensor(source, tensor, targetSize, &layout, error)) {
            return false;
        }
        const int roiWidth = qMax(1, qRound(static_cast<double>(sourceWidth) * bounded.width()));
        const int roiHeight = qMax(1, qRound(static_cast<double>(sourceHeight) * bounded.height()));
        const QVector<jcut::vulkan_detector::ScrfdDetection> raw =
            detector->inferFromTensor(tensor, layout, roiWidth, roiHeight, threshold, error);
        if (!preprocessor->finishPendingPreprocess(error)) {
            return false;
        }
        out.reserve(out.size() + raw.size());
        for (const auto& det : raw) {
            QRectF box(bounded.x() * sourceWidth + det.box.x(),
                       bounded.y() * sourceHeight + det.box.y(),
                       det.box.width(),
                       det.box.height());
            out.push_back({box, det.confidence});
        }
        return true;
    };
    if (!appendPass(QRectF(0.0, 0.0, 1.0, 1.0))) {
        return out;
    }
    if (tiledPass) {
        constexpr qreal kTileSize = 0.60;
        constexpr qreal kTileStep = 0.40;
        static const QRectF tileRois[] = {
            QRectF(0.00, 0.00, kTileSize, kTileSize),
            QRectF(kTileStep, 0.00, kTileSize, kTileSize),
            QRectF(0.00, kTileStep, kTileSize, kTileSize),
            QRectF(kTileStep, kTileStep, kTileSize, kTileSize)
        };
        for (const QRectF& roi : tileRois) {
            if (!appendPass(roi)) {
                return out;
            }
        }
    }
    if (vulkanMs) {
        *vulkanMs = static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
    }
    return out;
}

QVector<Detection> detectScrfdFromDecoderFrame(jcut::vulkan_detector::VulkanZeroCopyFaceDetector* preprocessor,
                                               jcut::vulkan_detector::VulkanScrfdNcnnFaceDetector* detector,
                                               VulkanHarnessContext* vk,
                                               jcut::vulkan_detector::VulkanDetectorFrameHandoff* handoff,
                                               const editor::FrameHandle& frame,
                                               const QString& paramPath,
                                               const QString& binPath,
                                               float threshold,
                                               int targetSize,
                                               bool tiledPass,
                                               bool suppressNcnnInfo,
                                               bool allowCpuUploadFallback,
                                               double* uploadMs,
                                               double* vulkanMs,
                                               bool* hardwareDirectUsed,
                                               QString* error)
{
    QVector<Detection> out;
    if (!preprocessor || !detector || !vk || !handoff || frame.isNull()) {
        if (error) *error = QStringLiteral("Invalid decoder frame for Vulkan SCRFD handoff.");
        return out;
    }
    if (!handoff->uploadFrame(frame, allowCpuUploadFallback, uploadMs, error)) {
        return out;
    }
    if (hardwareDirectUsed) {
        *hardwareDirectUsed =
            handoff->lastMode() == jcut::vulkan_detector::FrameHandoffMode::HardwareDirect;
    }
    const jcut::vulkan_detector::VulkanExternalImage source = handoff->externalImage();
    if (source.imageView == VK_NULL_HANDLE || !source.size.isValid()) {
        if (error) *error = QStringLiteral("Frame handoff produced invalid external image.");
        return out;
    }
    render_detail::OffscreenVulkanFrame detectorFrame;
    detectorFrame.physicalDevice = vk->physicalDevice;
    detectorFrame.device = vk->device;
    detectorFrame.queue = vk->queue;
    detectorFrame.queueFamilyIndex = vk->queueFamilyIndex;
    detectorFrame.imageView = source.imageView;
    detectorFrame.imageLayout = source.imageLayout;
    detectorFrame.size = source.size;
    detectorFrame.queueSupportsCompute = true;
    detectorFrame.valid = true;
    out = detectScrfdVulkanFrame(preprocessor,
                                 detector,
                                 vk,
                                 detectorFrame,
                                 paramPath,
                                 binPath,
                                 threshold,
                                 targetSize,
                                 tiledPass,
                                 suppressNcnnInfo,
                                 vulkanMs,
                                 error);
    if (!handoff->finishPendingUpload(nullptr, error)) {
        out.clear();
        return out;
    }
    return out;
}

struct PreparedDecoderDetectionSlot {
    VulkanHarnessContext context;
    jcut::vulkan_detector::VulkanDetectorFrameHandoff handoff;
    jcut::vulkan_detector::VulkanZeroCopyFaceDetector preprocessor;
    editor::FrameHandle decodedFrame;
    jcut::vulkan_detector::ScrfdTensorLayout scrfdLayout;
    int frameNumber = -1;
    int frameOffset = -1;
    QSize detectionFrameSize;
    double decoderUploadMs = 0.0;
    bool hardwareDirectHandoff = false;
    bool decoderVulkanUploadFallback = false;
    bool active = false;
};

bool prepareRes10DecoderFrame(jcut::vulkan_detector::VulkanZeroCopyFaceDetector* preprocessor,
                              jcut::vulkan_detector::VulkanRes10NcnnFaceDetector* detector,
                              VulkanHarnessContext* vk,
                              jcut::vulkan_detector::VulkanDetectorFrameHandoff* handoff,
                              const editor::FrameHandle& frame,
                              const QString& paramPath,
                              const QString& binPath,
                              bool suppressNcnnInfo,
                              bool allowCpuUploadFallback,
                              double* uploadMs,
                              bool* hardwareDirectUsed,
                              QSize* detectionFrameSize,
                              QString* error)
{
    if (!preprocessor || !detector || !vk || !handoff || frame.isNull()) {
        if (error) *error = QStringLiteral("Invalid decoder frame for Vulkan Res10 preparation.");
        return false;
    }
    if (!handoff->uploadFrame(frame, allowCpuUploadFallback, uploadMs, error)) {
        return false;
    }
    if (hardwareDirectUsed) {
        *hardwareDirectUsed =
            handoff->lastMode() == jcut::vulkan_detector::FrameHandoffMode::HardwareDirect;
    }
    const jcut::vulkan_detector::VulkanExternalImage source = handoff->externalImage();
    if (source.imageView == VK_NULL_HANDLE || !source.size.isValid()) {
        if (error) *error = QStringLiteral("Frame handoff produced invalid external image.");
        return false;
    }
    if (!preprocessor->isInitialized() &&
        !preprocessor->initialize(vk->detectorContext(), error)) {
        return false;
    }
    if (!detector->isInitialized()) {
        ScopedStderrSilencer silence(suppressNcnnInfo);
        if (!detector->initialize(vk->detectorContext(), paramPath, binPath, error)) {
            return false;
        }
    }
    if (!vk->ensureDetectorBuffers(preprocessor->tensorSpec().byteSize(), 1, error)) {
        return false;
    }
    const jcut::vulkan_detector::VulkanTensorBuffer tensor{
        vk->tensorBuffer,
        preprocessor->tensorSpec().byteSize()
    };
    if (!preprocessor->preprocessToTensor(source, tensor, error)) {
        return false;
    }
    if (detectionFrameSize) {
        *detectionFrameSize = source.size;
    }
    return true;
}

QVector<Detection> finalizePreparedRes10DecoderFrame(jcut::vulkan_detector::VulkanZeroCopyFaceDetector* preprocessor,
                                                     jcut::vulkan_detector::VulkanRes10NcnnFaceDetector* detector,
                                                     VulkanHarnessContext* vk,
                                                     jcut::vulkan_detector::VulkanDetectorFrameHandoff* handoff,
                                                     const QSize& detectionFrameSize,
                                                     float threshold,
                                                     double* vulkanMs,
                                                     QString* error)
{
    QVector<Detection> out;
    if (!preprocessor || !detector || !vk || !handoff || !detectionFrameSize.isValid()) {
        if (error) *error = QStringLiteral("Invalid prepared Vulkan Res10 decoder frame.");
        return out;
    }
    QElapsedTimer timer;
    timer.start();
    const jcut::vulkan_detector::VulkanTensorBuffer tensor{
        vk->tensorBuffer,
        preprocessor->tensorSpec().byteSize()
    };
    const QVector<jcut::vulkan_detector::Res10Detection> raw =
        detector->inferFromTensor(tensor,
                                  detectionFrameSize.width(),
                                  detectionFrameSize.height(),
                                  threshold,
                                  error);
    if (!preprocessor->finishPendingPreprocess(error) ||
        !handoff->finishPendingUpload(nullptr, error)) {
        out.clear();
        return out;
    }
    out.reserve(raw.size());
    for (const auto& det : raw) {
        out.push_back({det.box, det.confidence});
    }
    if (vulkanMs) {
        *vulkanMs = static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
    }
    return out;
}

bool prepareScrfdDecoderFrame(jcut::vulkan_detector::VulkanZeroCopyFaceDetector* preprocessor,
                              jcut::vulkan_detector::VulkanScrfdNcnnFaceDetector* detector,
                              VulkanHarnessContext* vk,
                              jcut::vulkan_detector::VulkanDetectorFrameHandoff* handoff,
                              const editor::FrameHandle& frame,
                              const QString& paramPath,
                              const QString& binPath,
                              int targetSize,
                              bool suppressNcnnInfo,
                              bool allowCpuUploadFallback,
                              jcut::vulkan_detector::ScrfdTensorLayout* layout,
                              double* uploadMs,
                              bool* hardwareDirectUsed,
                              QSize* detectionFrameSize,
                              QString* error)
{
    if (!preprocessor || !detector || !vk || !handoff || frame.isNull() || !layout) {
        if (error) *error = QStringLiteral("Invalid decoder frame for Vulkan SCRFD preparation.");
        return false;
    }
    if (!handoff->uploadFrame(frame, allowCpuUploadFallback, uploadMs, error)) {
        return false;
    }
    if (hardwareDirectUsed) {
        *hardwareDirectUsed =
            handoff->lastMode() == jcut::vulkan_detector::FrameHandoffMode::HardwareDirect;
    }
    const jcut::vulkan_detector::VulkanExternalImage source = handoff->externalImage();
    if (source.imageView == VK_NULL_HANDLE || !source.size.isValid()) {
        if (error) *error = QStringLiteral("Frame handoff produced invalid external image.");
        return false;
    }
    if (!preprocessor->isInitialized() &&
        !preprocessor->initialize(vk->detectorContext(), error)) {
        return false;
    }
    if (!detector->isInitialized()) {
        ScopedStderrSilencer silence(suppressNcnnInfo);
        if (!detector->initialize(vk->detectorContext(), paramPath, binPath, error)) {
            return false;
        }
    }
    const int sourceWidth = qMax(1, source.size.width());
    const int sourceHeight = qMax(1, source.size.height());
    targetSize = qMax(320, targetSize);
    float scale = 1.0f;
    int resizedW = sourceWidth;
    int resizedH = sourceHeight;
    if (resizedW > resizedH) {
        scale = static_cast<float>(targetSize) / static_cast<float>(resizedW);
        resizedW = targetSize;
        resizedH = qMax(1, qRound(static_cast<float>(resizedH) * scale));
    } else {
        scale = static_cast<float>(targetSize) / static_cast<float>(resizedH);
        resizedH = targetSize;
        resizedW = qMax(1, qRound(static_cast<float>(resizedW) * scale));
    }
    const VkDeviceSize tensorBytes =
        static_cast<VkDeviceSize>(((resizedW + 31) / 32) * 32) *
        static_cast<VkDeviceSize>(((resizedH + 31) / 32) * 32) *
        static_cast<VkDeviceSize>(3 * sizeof(float));
    if (!vk->ensureDetectorBuffers(tensorBytes, 1, error)) {
        return false;
    }
    const jcut::vulkan_detector::VulkanTensorBuffer tensor{
        vk->tensorBuffer,
        vk->tensorSize
    };
    if (!preprocessor->preprocessScrfdToTensor(source, tensor, targetSize, layout, error)) {
        return false;
    }
    if (detectionFrameSize) {
        *detectionFrameSize = source.size;
    }
    return true;
}

QVector<Detection> finalizePreparedScrfdDecoderFrame(jcut::vulkan_detector::VulkanZeroCopyFaceDetector* preprocessor,
                                                     jcut::vulkan_detector::VulkanScrfdNcnnFaceDetector* detector,
                                                     VulkanHarnessContext* vk,
                                                     jcut::vulkan_detector::VulkanDetectorFrameHandoff* handoff,
                                                     const jcut::vulkan_detector::ScrfdTensorLayout& layout,
                                                     const QSize& detectionFrameSize,
                                                     float threshold,
                                                     double* vulkanMs,
                                                     QString* error)
{
    QVector<Detection> out;
    if (!preprocessor || !detector || !vk || !handoff || !detectionFrameSize.isValid()) {
        if (error) *error = QStringLiteral("Invalid prepared Vulkan SCRFD decoder frame.");
        return out;
    }
    QElapsedTimer timer;
    timer.start();
    const jcut::vulkan_detector::VulkanTensorBuffer tensor{
        vk->tensorBuffer,
        vk->tensorSize
    };
    const QVector<jcut::vulkan_detector::ScrfdDetection> raw =
        detector->inferFromTensor(tensor,
                                  layout,
                                  detectionFrameSize.width(),
                                  detectionFrameSize.height(),
                                  threshold,
                                  error);
    if (!preprocessor->finishPendingPreprocess(error) ||
        !handoff->finishPendingUpload(nullptr, error)) {
        out.clear();
        return out;
    }
    out.reserve(raw.size());
    for (const auto& det : raw) {
        out.push_back({det.box, det.confidence});
    }
    if (vulkanMs) {
        *vulkanMs = static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
    }
    return out;
}

#if JCUT_HAVE_OPENCV
bool hasVulkanDnnTarget()
{
    const std::vector<cv::dnn::Target> targets =
        cv::dnn::getAvailableTargets(cv::dnn::DNN_BACKEND_VKCOM);
    return std::find(targets.begin(), targets.end(), cv::dnn::DNN_TARGET_VULKAN) != targets.end();
}

bool initializeTrainedVulkanDnn(cv::dnn::Net* net, QString* error)
{
    if (!net) {
        if (error) *error = QStringLiteral("Invalid trained Vulkan DNN runtime.");
        return false;
    }
    if (!hasVulkanDnnTarget()) {
        if (error) *error = QStringLiteral("OpenCV VKCOM/Vulkan DNN target is unavailable in this build/runtime.");
        return false;
    }
    QString proto;
    QString model;
    if (!jcut::facestream::ensureFaceDnnModel(QDir::currentPath(), &proto, &model)) {
        if (error) *error = QStringLiteral("OpenCV Res10 SSD face detector assets are missing or could not be downloaded.");
        return false;
    }
    try {
        *net = cv::dnn::readNetFromCaffe(proto.toStdString(), model.toStdString());
        net->setPreferableBackend(cv::dnn::DNN_BACKEND_VKCOM);
        net->setPreferableTarget(cv::dnn::DNN_TARGET_VULKAN);
    } catch (const cv::Exception& e) {
        if (error) *error = QStringLiteral("Failed to initialize trained Vulkan DNN: %1").arg(e.what());
        return false;
    }
    return true;
}

QVector<Detection> detectTrainedVulkanDnn(cv::dnn::Net* net,
                                          const cv::Mat& bgr,
                                          float threshold,
                                          double* inferenceMs,
                                          QString* error)
{
    QVector<Detection> out;
    if (!net || bgr.empty()) {
        return out;
    }
    QElapsedTimer timer;
    timer.start();
    try {
        cv::Mat blob = cv::dnn::blobFromImage(
            bgr, 1.0, cv::Size(300, 300), cv::Scalar(104.0, 177.0, 123.0), false, false);
        net->setInput(blob);
        const cv::Mat detections = net->forward();
        if (detections.dims != 4 || detections.size[2] <= 0 || detections.size[3] < 7) {
            if (error) *error = QStringLiteral("Unexpected trained Vulkan DNN output shape.");
            return out;
        }
        const int width = bgr.cols;
        const int height = bgr.rows;
        for (int i = 0; i < detections.size[2]; ++i) {
            const float* row = detections.ptr<float>(0, 0, i);
            const float confidence = row[2];
            if (confidence < threshold) {
                continue;
            }
            int x1 = static_cast<int>(row[3] * width);
            int y1 = static_cast<int>(row[4] * height);
            int x2 = static_cast<int>(row[5] * width);
            int y2 = static_cast<int>(row[6] * height);
            x1 = qBound(0, x1, qMax(0, width - 1));
            y1 = qBound(0, y1, qMax(0, height - 1));
            x2 = qBound(0, x2, qMax(0, width - 1));
            y2 = qBound(0, y2, qMax(0, height - 1));
            QRectF box(x1, y1, qMax(0, x2 - x1), qMax(0, y2 - y1));
            box = box.intersected(QRectF(0, 0, width, height));
            if (box.width() >= 8.0 && box.height() >= 8.0) {
                out.push_back({box, confidence});
            }
        }
    } catch (const cv::Exception& e) {
        if (error) *error = QStringLiteral("Trained Vulkan DNN inference failed: %1").arg(e.what());
        return {};
    }
    if (inferenceMs) {
        *inferenceMs = static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
    }
    std::sort(out.begin(), out.end(), [](const Detection& a, const Detection& b) {
        return a.confidence > b.confidence;
    });
    return out;
}
#endif

QImage buildPreview(QImage image,
                    const QVector<Track>& tracks,
                    const QVector<Detection>& detections,
                    const QRectF& roiRect)
{
    QVector<QRect> boxes;
    if (!tracks.isEmpty()) {
        boxes.reserve(tracks.size());
        for (const Track& track : tracks) {
            if (track.id < 0 || !track.box.isValid() || track.box.isEmpty()) {
                continue;
            }
            boxes.push_back(track.box.toAlignedRect());
        }
    } else {
        boxes.reserve(detections.size());
        for (const Detection& detection : detections) {
            boxes.push_back(detection.box.toAlignedRect());
        }
    }
    return jcut::facestream::buildScanPreview(image, boxes, boxes.size(), roiRect);
}

bool sendPreviewFrame(QLocalSocket* socket, const QImage& image)
{
    if (!socket || socket->state() != QLocalSocket::ConnectedState || image.isNull()) {
        return false;
    }
    const QImage argb = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QByteArray payload;
    payload.resize(argb.sizeInBytes());
    std::memcpy(payload.data(), argb.constBits(), static_cast<size_t>(payload.size()));
    QDataStream stream(socket);
    stream.setVersion(QDataStream::Qt_6_0);
    stream << quint32(argb.width()) << quint32(argb.height()) << quint32(payload.size());
    const qint64 written = socket->write(payload);
    socket->flush();
    socket->waitForBytesWritten(100);
    return written == payload.size();
}

bool writeJson(const QString& path, const QJsonObject& object)
{
    return jcut::jsonio::writeJsonFile(path, object, true, nullptr);
}

bool writeBinaryJsonObject(const QString& path, const QJsonObject& object)
{
    return jcut::jsonio::writeBinaryJsonObject(path, object, 0x4A435554, 1, nullptr);
}

bool appendBinaryJsonRecord(QFile* file, const QJsonObject& object)
{
    return jcut::jsonio::appendBinaryJsonRecord(file, object, 0x4A465352, 1, nullptr);
}

struct FaceStreamResumeState {
    QSet<int> completedFrames;
    QJsonArray frameRows;
    QJsonArray rawDetectionFrames;
    QHash<int, int> rawDetectionFrameIndexByFrame;
    QHash<int, QJsonArray> trackDetectionsByFrame;
    int processed = 0;
    int totalDetections = 0;
    int appVulkanFramePathFrames = 0;
    int decoderDirectHandoffFrames = 0;
    int decoderVulkanUploadFallbackFrames = 0;
    int hardwareDirectHandoffFrames = 0;
    int hardwareInteropProbeSupportedFrames = 0;
    int hardwareInteropProbeFailedFrames = 0;
    int hardwareFrames = 0;
    int cpuFrames = 0;
    double renderDecodeMsTotal = 0.0;
    double renderCompositeMsTotal = 0.0;
    double renderReadbackMsTotal = 0.0;
    double decoderUploadMsTotal = 0.0;
    double vulkanDetectMsTotal = 0.0;
    double ncnnInputMsTotal = 0.0;
    double ncnnExtractMsTotal = 0.0;
    double ncnnExtractLevel8MsTotal = 0.0;
    double ncnnExtractLevel16MsTotal = 0.0;
    double ncnnExtractLevel32MsTotal = 0.0;
    double ncnnPostMsTotal = 0.0;
    double ncnnTotalMsTotal = 0.0;
};

bool loadFaceStreamResume(const QString& path,
                          const QString& videoPath,
                          const QString& backend,
                          int startFrame,
                          int endFrame,
                          FaceStreamResumeState* state,
                          QString* error)
{
    if (!state || !QFileInfo::exists(path)) {
        return true;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("Failed to open existing facestream checkpoint: %1").arg(path);
        return false;
    }
    bool sawMeta = false;
    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_6_0);
    while (!stream.atEnd()) {
        quint32 magic = 0;
        quint32 version = 0;
        quint32 compressedSize = 0;
        stream >> magic;
        stream >> version;
        stream >> compressedSize;
        if (stream.status() != QDataStream::Ok) {
            break;
        }
        if (magic != 0x4A465352 || version != 1) {
            if (error) {
                *error = QStringLiteral("Invalid facestream checkpoint record header.");
            }
            return false;
        }
        QByteArray compressed;
        compressed.resize(static_cast<int>(compressedSize));
        if (compressedSize > 0) {
            const int bytesRead = stream.readRawData(compressed.data(), static_cast<int>(compressedSize));
            if (bytesRead != static_cast<int>(compressedSize)) {
                if (error) {
                    *error = QStringLiteral("Truncated facestream checkpoint record.");
                }
                return false;
            }
        }
        QJsonObject object;
        if (!jcut::jsonio::parseRecordPayload(qUncompress(compressed), &object, nullptr)) {
            continue;
        }
        const QString type = object.value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("meta")) {
            sawMeta = true;
            if (object.value(QStringLiteral("video")).toString() != videoPath ||
                object.value(QStringLiteral("backend")).toString() != backend) {
                if (error) {
                    *error = QStringLiteral("Existing facestream checkpoint is for a different video/backend.");
                }
                return false;
            }
            continue;
        }
        if (type != QStringLiteral("frame")) {
            continue;
        }
        if (sawMeta &&
            (object.value(QStringLiteral("video")).toString(videoPath) != videoPath ||
             object.value(QStringLiteral("backend")).toString(backend) != backend)) {
            continue;
        }
        const int frameNumber = object.value(QStringLiteral("frame")).toInt(-1);
        if (frameNumber < startFrame || frameNumber > endFrame || state->completedFrames.contains(frameNumber)) {
            continue;
        }
        state->completedFrames.insert(frameNumber);
        ++state->processed;
        const int detectionCount = object.value(QStringLiteral("detections")).toInt(0);
        state->totalDetections += detectionCount;
        if (object.value(QStringLiteral("app_vulkan_frame_path")).toBool(false)) ++state->appVulkanFramePathFrames;
        if (object.value(QStringLiteral("decoder_direct_handoff")).toBool(false)) ++state->decoderDirectHandoffFrames;
        if (object.value(QStringLiteral("decoder_vulkan_upload_fallback")).toBool(false)) ++state->decoderVulkanUploadFallbackFrames;
        if (object.value(QStringLiteral("hardware_direct_handoff")).toBool(false)) ++state->hardwareDirectHandoffFrames;
        if (object.value(QStringLiteral("hardware_interop_probe_supported")).toBool(false)) ++state->hardwareInteropProbeSupportedFrames;
        if (object.value(QStringLiteral("hardware_interop_probe_failed")).toBool(false)) ++state->hardwareInteropProbeFailedFrames;
        if (object.value(QStringLiteral("hardware_frame")).toBool(false)) ++state->hardwareFrames;
        if (object.value(QStringLiteral("cpu_frame")).toBool(false)) ++state->cpuFrames;
        state->renderDecodeMsTotal += object.value(QStringLiteral("app_render_decode_ms")).toDouble(0.0);
        state->renderCompositeMsTotal += object.value(QStringLiteral("app_render_composite_ms")).toDouble(0.0);
        state->renderReadbackMsTotal += object.value(QStringLiteral("app_render_readback_ms")).toDouble(0.0);
        state->decoderUploadMsTotal += object.value(QStringLiteral("decoder_vulkan_upload_ms")).toDouble(0.0);
        state->vulkanDetectMsTotal += object.value(QStringLiteral("vulkan_zero_copy_detection_ms")).toDouble(0.0);

        state->frameRows.append(QJsonObject{
            {QStringLiteral("frame"), frameNumber},
            {QStringLiteral("detector"), object.value(QStringLiteral("detector")).toString(backend)},
            {QStringLiteral("detections"), detectionCount},
            {QStringLiteral("tracks"), object.value(QStringLiteral("tracks")).toInt(0)},
            {QStringLiteral("app_vulkan_frame_path"), object.value(QStringLiteral("app_vulkan_frame_path")).toBool(false)},
            {QStringLiteral("app_render_decode_ms"), object.value(QStringLiteral("app_render_decode_ms")).toDouble(0.0)},
            {QStringLiteral("app_render_texture_ms"), object.value(QStringLiteral("app_render_texture_ms")).toDouble(0.0)},
            {QStringLiteral("app_render_composite_ms"), object.value(QStringLiteral("app_render_composite_ms")).toDouble(0.0)},
            {QStringLiteral("app_render_readback_ms"), object.value(QStringLiteral("app_render_readback_ms")).toDouble(0.0)},
            {QStringLiteral("vulkan_zero_copy_detection_ms"), object.value(QStringLiteral("vulkan_zero_copy_detection_ms")).toDouble(0.0)},
            {QStringLiteral("decoder_vulkan_upload_ms"), object.value(QStringLiteral("decoder_vulkan_upload_ms")).toDouble(0.0)},
            {QStringLiteral("decoder_vulkan_upload_fallback"), object.value(QStringLiteral("decoder_vulkan_upload_fallback")).toBool(false)},
            {QStringLiteral("hardware_direct_handoff"), object.value(QStringLiteral("hardware_direct_handoff")).toBool(false)},
            {QStringLiteral("hardware_direct_attempt_reason"), object.value(QStringLiteral("hardware_direct_attempt_reason")).toString()},
            {QStringLiteral("qimage_materialized"), object.value(QStringLiteral("qimage_materialized")).toBool(false)}
        });
        const QJsonArray detectionBoxes = object.value(QStringLiteral("detection_boxes")).toArray();
        const QJsonObject firstDetection = detectionBoxes.isEmpty()
            ? QJsonObject{}
            : detectionBoxes.at(0).toObject();
        state->rawDetectionFrames.append(QJsonObject{
            {QStringLiteral("frame"), frameNumber},
            {QStringLiteral("detector"), object.value(QStringLiteral("detector")).toString(backend)},
            {QStringLiteral("frame_width"), firstDetection.value(QStringLiteral("frame_width")).toInt()},
            {QStringLiteral("frame_height"), firstDetection.value(QStringLiteral("frame_height")).toInt()},
            {QStringLiteral("detection_count"), detectionCount},
            {QStringLiteral("detections"), detectionBoxes}
        });
        state->trackDetectionsByFrame.insert(frameNumber, object.value(QStringLiteral("track_detections")).toArray());

    }
    return true;
}

} // namespace

static int runVulkanFacestreamOffscreenWithArgv(int argc, char** argv)
{
    QApplication* appPtr = qobject_cast<QApplication*>(QCoreApplication::instance());
    std::unique_ptr<QApplication> ownedApp;
    if (!appPtr) {
        ownedApp = std::make_unique<QApplication>(argc, argv);
        appPtr = ownedApp.get();
    }
    if (!appPtr) {
        std::cerr << "Failed to initialize QApplication for FaceStream generator.\n";
        return 2;
    }
    editor::installFfmpegLogFilter();
    Options options;
    if (!parseArgs(argc, argv, &options)) {
        return 2;
    }
    if (!QFileInfo::exists(options.videoPath)) {
        std::cerr << "Video not found: " << options.videoPath.toStdString() << "\n";
        return 2;
    }
    if (options.preflight) {
        const bool platformIsOffscreen =
            QString::fromLocal8Bit(qgetenv("QT_QPA_PLATFORM")).compare(QStringLiteral("offscreen"),
                                                                       Qt::CaseInsensitive) == 0;
        if (platformIsOffscreen) {
            std::cerr << "--preflight requires a GUI-capable Qt platform.\n";
            return 2;
        }
        jcut::facestream::DetectorRuntimeSettings detectorSettings;
        detectorSettings.stride = options.stride;
        detectorSettings.maxDetections = options.maxDetections;
        detectorSettings.scrfdTargetSize = options.scrfdTargetSize;
        detectorSettings.maxFacesPerFrame = options.maxFacesPerFrame;
        detectorSettings.scrfdModelVariant =
            jcut::facestream::normalizeScrfdModelVariantId(options.scrfdModelVariant);
        detectorSettings.threshold = options.threshold;
        detectorSettings.nmsIouThreshold = options.nmsIouThreshold;
        detectorSettings.trackMatchIouThreshold = options.trackMatchIouThreshold;
        detectorSettings.newTrackMinConfidence = options.newTrackMinConfidence;
        detectorSettings.primaryFaceOnly = options.primaryFaceOnly;
        detectorSettings.smallFaceFallback = options.smallFaceFallback;
        detectorSettings.scrfdTiled = options.scrfdTiled;

        const QString detectorSettingsPath =
            jcut::facestream::detectorSettingsPathForVideo(options.videoPath);
        jcut::facestream::loadDetectorRuntimeSettingsFile(detectorSettingsPath, &detectorSettings, nullptr);
        const jcut::facestream::FaceStreamPreflightDialogResult preflight =
            jcut::facestream::runFaceStreamPreflightDialog(
                &detectorSettings,
                options.detector,
                options.scrfdTargetSize,
                detectorSettingsPath,
                jcut::facestream::FaceStreamPreflightDialogOptions{
                    QStringLiteral("JCut DNN FaceStream Generator"),
                    QStringLiteral("This flow runs the standalone FaceStream detector directly against the selected media.\n\n"
                                   "Detector: Vulkan-native face detection pipeline. Track processing runs later."),
                    QStringLiteral("The saved detector settings file is reused by both the editor and the standalone runner for this source video."),
                    QStringLiteral("Run"),
                    QStringLiteral("Cancel"),
                    QSize(760, 420),
                    true,
                    options.livePreview,
                    false,
                    !options.clipJsonPath.trimmed().isEmpty(),
                    options.applyClipGrading,
                    QStringLiteral("Apply clip grading during detection")
                });
        if (!preflight.accepted) {
            return 0;
        }
        if (!preflight.saveError.trimmed().isEmpty()) {
            std::cerr << preflight.saveError.toStdString() << "\n";
            return 2;
        }
        options.livePreview = preflight.livePreview;
        options.applyClipGrading = preflight.applyClipGrading;
        options.stride = detectorSettings.stride;
        options.maxDetections = detectorSettings.maxDetections;
        options.scrfdTargetSize = detectorSettings.scrfdTargetSize;
        options.maxFacesPerFrame = detectorSettings.maxFacesPerFrame;
        options.scrfdModelVariant =
            jcut::facestream::normalizeScrfdModelVariantId(detectorSettings.scrfdModelVariant);
        options.threshold = detectorSettings.threshold;
        options.nmsIouThreshold = detectorSettings.nmsIouThreshold;
        options.trackMatchIouThreshold = detectorSettings.trackMatchIouThreshold;
        options.newTrackMinConfidence = detectorSettings.newTrackMinConfidence;
        options.primaryFaceOnly = detectorSettings.primaryFaceOnly;
        options.smallFaceFallback = detectorSettings.smallFaceFallback;
        options.scrfdTiled = detectorSettings.scrfdTiled;
    }
    auto applyScrfdModelVariantFromSettingsFile = [&](const QString& path) {
        if (path.trimmed().isEmpty()) {
            return;
        }
        QJsonObject object;
        if (!jcut::jsonio::readJsonFile(path, &object, nullptr)) {
            return;
        }
        if (object.contains(QStringLiteral("scrfd_model_variant"))) {
            options.scrfdModelVariant =
                jcut::facestream::normalizeScrfdModelVariantId(
                    object.value(QStringLiteral("scrfd_model_variant")).toString(options.scrfdModelVariant));
        }
    };
    if (!options.paramsFile.trimmed().isEmpty()) {
        applyScrfdModelVariantFromSettingsFile(options.paramsFile);
    }
    applyScrfdModelVariantFromSettingsFile(
        jcut::facestream::detectorSettingsPathForVideo(options.videoPath));
    qputenv("JCUT_ALLOW_HEADLESS_HARDWARE_DECODE", "1");
    QDir().mkpath(options.outputDir);
    const bool platformIsOffscreen =
        QString::fromLocal8Bit(qgetenv("QT_QPA_PLATFORM")).compare(QStringLiteral("offscreen"),
                                                                   Qt::CaseInsensitive) == 0;
    std::unique_ptr<ImGuiPreviewWindow> livePreviewWindow;

    editor::setDebugDecodePreference(options.decodePreference);
    editor::DecoderContext decoder(options.videoPath);
    if (!decoder.initialize()) {
        std::cerr << "Failed to initialize decoder: " << options.videoPath.toStdString() << "\n";
        return 2;
    }
    TimelineClip sourceClip;
    sourceClip.id = QStringLiteral("facestream-offscreen-source");
    sourceClip.filePath = options.videoPath;
    sourceClip.mediaType = ClipMediaType::Video;
    sourceClip.videoEnabled = true;
    sourceClip.audioEnabled = false;
    sourceClip.hasAudio = false;
    sourceClip.startFrame = options.startFrame;
    sourceClip.sourceInFrame = options.startFrame;
    const int64_t decoderDurationFrames = qMax<int64_t>(0, decoder.info().durationFrames);
    const int64_t availableFrames = qMax<int64_t>(1, decoderDurationFrames - options.startFrame);
    const int64_t targetFrames = options.maxFrames > 0
        ? qMin<int64_t>(static_cast<int64_t>(options.maxFrames), availableFrames)
        : availableFrames;
    sourceClip.durationFrames = qMax<int64_t>(1, targetFrames);
    sourceClip.sourceDurationFrames = qMax<int64_t>(1, targetFrames);
    sourceClip.sourceFps = 30.0;
    sourceClip.playbackRate = 1.0;
    if (options.applyClipGrading && options.clipJsonPath.trimmed().isEmpty()) {
        std::cerr << "--apply-clip-grading requires --clip-json.\n";
        return 2;
    }
    const TimelineClip baseSourceClip = sourceClip;
    TimelineClip gradedSourceClip = sourceClip;
    if (!options.clipJsonPath.trimmed().isEmpty()) {
        TimelineClip gradedClip;
        QString clipLoadError;
        if (!loadClipFromJsonPath(options.clipJsonPath, &gradedClip, &clipLoadError)) {
            std::cerr << clipLoadError.toStdString() << "\n";
            return 2;
        }
        gradedSourceClip.brightness = gradedClip.brightness;
        gradedSourceClip.contrast = gradedClip.contrast;
        gradedSourceClip.saturation = gradedClip.saturation;
        gradedSourceClip.opacity = gradedClip.opacity;
        gradedSourceClip.gradingKeyframes = gradedClip.gradingKeyframes;
        gradedSourceClip.opacityKeyframes = gradedClip.opacityKeyframes;
    }
    auto syncSourceClipGrading = [&]() {
        const TimelineClip& effectiveClip =
            options.applyClipGrading ? gradedSourceClip : baseSourceClip;
        sourceClip.brightness = effectiveClip.brightness;
        sourceClip.contrast = effectiveClip.contrast;
        sourceClip.saturation = effectiveClip.saturation;
        sourceClip.opacity = effectiveClip.opacity;
        sourceClip.gradingKeyframes = effectiveClip.gradingKeyframes;
        sourceClip.opacityKeyframes = effectiveClip.opacityKeyframes;
    };
    syncSourceClipGrading();
    jcut::facestream::VulkanFrameProvider appFrameProvider;
    const QSize renderSize = decoder.info().frameSize.isValid()
        ? decoder.info().frameSize
        : QSize(1920, 1080);
    sourceClip.sourceFrameSize = renderSize;
    if (options.livePreview && !platformIsOffscreen) {
        livePreviewWindow = std::make_unique<ImGuiPreviewWindow>();
        if (!livePreviewWindow->initialize(
                QStringLiteral("JCut DNN FaceStream Generator Preview"),
                QSize(1080, 720))) {
            std::cerr << "Failed to initialize Dear ImGui preview window: "
                      << livePreviewWindow->failureReason().toStdString() << "\n";
            return 2;
        }
        livePreviewWindow->setStatusText(QStringLiteral("Waiting for JCut DNN FaceStream preview..."));
    }
    QLocalSocket previewSocket;
    QLocalSocket* previewSocketPtr = nullptr;
    if (!options.previewSocket.isEmpty()) {
        previewSocket.connectToServer(options.previewSocket);
        if (previewSocket.waitForConnected(3000)) {
            previewSocketPtr = &previewSocket;
        } else {
            std::cerr << "Failed to connect preview socket: "
                      << options.previewSocket.toStdString() << "\n";
        }
    }
    const bool preferDecoderDirectDetection =
        !options.materializedGenerateFacestream &&
        !options.applyClipGrading;
    if (preferDecoderDirectDetection) {
        if (previewSocketPtr) {
            std::cerr << "Preview socket output requires QImage readback and is disabled for decoder-direct zero-copy detection.\n";
            previewSocket.disconnectFromServer();
            previewSocket.close();
            previewSocketPtr = nullptr;
        }
        if (options.writePreviewFiles) {
            std::cerr << "Preview file output requires QImage readback and is disabled for decoder-direct zero-copy detection.\n";
            options.writePreviewFiles = false;
        }
    }
    if (options.requireZeroCopy && !preferDecoderDirectDetection) {
        std::cerr << "--require-zero-copy requires the decoder-direct ungraded path. "
                     "Disable clip grading and materialized compatibility mode.\n";
        return 2;
    }
    const bool previewRequested = static_cast<bool>(livePreviewWindow) || previewSocketPtr || options.writePreviewFiles;
    bool previewRequiresSynchronizedDetection =
        static_cast<bool>(livePreviewWindow) || (previewSocketPtr != nullptr);
    const int effectivePreviewStride =
        livePreviewWindow ? 1 : qMax(1, options.previewStride);
    bool previewPipelinePrimed = !previewRequested;
    QString lastPreviewStatusText;
    auto setPreviewStatusText = [&](const QString& text) {
        if (!livePreviewWindow || text == lastPreviewStatusText) {
            return;
        }
        livePreviewWindow->setStatusText(text);
        lastPreviewStatusText = text;
    };
    bool previewFailureLogged = false;
    auto disableSynchronizedPreview = [&](const QString& reason) {
        if (!previewFailureLogged) {
            std::cerr << "Preview synchronization disabled";
            if (!reason.trimmed().isEmpty()) {
                std::cerr << ": " << reason.toStdString();
            }
            std::cerr << "\n";
            previewFailureLogged = true;
        }
        previewRequiresSynchronizedDetection = false;
        previewPipelinePrimed = true;
        livePreviewWindow.reset();
    };

    QString error;
#if JCUT_HAVE_OPENCV
    cv::CascadeClassifier faceCascade;
    cv::CascadeClassifier faceCascadeAlt;
    cv::CascadeClassifier faceCascadeProfile;
    if (options.materializedGenerateFacestream) {
        const QString cascadePath = jcut::facestream::findCascadeFile(QStringLiteral("haarcascade_frontalface_default.xml"));
        if (cascadePath.isEmpty() || !faceCascade.load(cascadePath.toStdString())) {
            std::cerr << "Failed to load Haar cascade for materialized Generate FaceStream scan.\n";
            return 2;
        }
        const QString altCascadePath = jcut::facestream::findCascadeFile(QStringLiteral("haarcascade_frontalface_alt2.xml"));
        if (!altCascadePath.isEmpty()) {
            faceCascadeAlt.load(altCascadePath.toStdString());
        }
        const QString profileCascadePath = jcut::facestream::findCascadeFile(QStringLiteral("haarcascade_profileface.xml"));
        if (!profileCascadePath.isEmpty()) {
            faceCascadeProfile.load(profileCascadePath.toStdString());
        }
    }
#else
    if (options.materializedGenerateFacestream) {
        std::cerr << "OpenCV is not enabled in this build; materialized Generate FaceStream scan is unavailable.\n";
        return 2;
    }
#endif
    const bool zeroCopyVulkanDetector = !options.materializedGenerateFacestream;
    const QString normalizedScrfdModelVariant =
        jcut::facestream::normalizeScrfdModelVariantId(options.scrfdModelVariant);
    const QString res10ParamPath = findRes10NcnnModelFile(
        options.res10ParamPath, QStringLiteral("res10_300x300_ssd_ncnn.param"));
    const QString res10BinPath = findRes10NcnnModelFile(
        options.res10BinPath, QStringLiteral("res10_300x300_ssd_ncnn.bin"));
    QString scrfdParamPath;
    QString scrfdBinPath;
    if (options.scrfdDetector) {
        if (!options.scrfdParamPath.trimmed().isEmpty() || !options.scrfdBinPath.trimmed().isEmpty()) {
            scrfdParamPath = findRes10NcnnModelFile(
                options.scrfdParamPath, scrfdModelFileName(normalizedScrfdModelVariant, QStringLiteral(".param")));
            scrfdBinPath = findRes10NcnnModelFile(
                options.scrfdBinPath, scrfdModelFileName(normalizedScrfdModelVariant, QStringLiteral(".bin")));
            if (!QFileInfo::exists(scrfdParamPath) || !QFileInfo::exists(scrfdBinPath)) {
                std::cerr << "Explicit SCRFD model paths are missing for variant "
                          << normalizedScrfdModelVariant.toStdString()
                          << ". param=" << scrfdParamPath.toStdString()
                          << " bin=" << scrfdBinPath.toStdString() << "\n";
                return 2;
            }
        } else {
            if (!jcut::facestream::ensureScrfdModelVariantAssets(
                    normalizedScrfdModelVariant, &scrfdParamPath, &scrfdBinPath, &error)) {
                std::cerr << error.toStdString() << "\n";
                return 2;
            }
        }
    }
    if (options.requireZeroCopy && options.materializedGenerateFacestream) {
        std::cerr << "Materialized Generate FaceStream mode cannot satisfy --require-zero-copy.\n";
        return 2;
    }
    if (zeroCopyVulkanDetector && !options.heuristicZeroCopyDetector && !options.scrfdDetector) {
        std::cout << "detector=res10_ssd_ncnn_vulkan_zero_copy_v1"
                  << " name=\"JCut DNN FaceStream Generator\""
                  << " inference_backend=ncnn_vulkan"
                  << " model_param=\"" << res10ParamPath.toStdString() << "\""
                  << " model_bin=\"" << res10BinPath.toStdString() << "\""
                  << " qimage_materialized=0\n";
    } else if (options.heuristicZeroCopyDetector) {
        std::cout << "detector=native_jcut_heuristic_zero_copy_v1"
                  << " name=\"JCut DNN FaceStream Generator\""
                  << " decode=hardware_zero_copy"
                  << " qimage_materialized=0\n";
    } else if (options.scrfdDetector) {
        std::cout << "detector=scrfd_" << normalizedScrfdModelVariant.toStdString() << "_ncnn_vulkan_zero_copy_v1"
                  << " name=\"JCut SCRFD FaceStream Generator\""
                  << " inference_backend=ncnn_vulkan"
                  << " scrfd_model_variant=\"" << normalizedScrfdModelVariant.toStdString() << "\""
                  << " model_param=\"" << scrfdParamPath.toStdString() << "\""
                  << " model_bin=\"" << scrfdBinPath.toStdString() << "\""
                  << " qimage_materialized=0"
                  << " zero_copy=1\n";
    }
    const QString backend = backendIdForOptions(options);

    int decoded = 0;
    int processed = 0;
    int cpuFrames = 0;
    int hardwareFrames = 0;
    int totalDetections = 0;
    int previewWritten = 0;
    int appVulkanFramePathFrames = 0;
    int decoderDirectHandoffFrames = 0;
    int decoderVulkanUploadFallbackFrames = 0;
    int hardwareDirectHandoffFrames = 0;
    int hardwareInteropProbeSupportedFrames = 0;
    int hardwareInteropProbeFailedFrames = 0;
    QString lastHardwareInteropProbeReason;
    QString lastHardwareInteropProbePath;
    QString lastHardwareDirectAttemptReason;
    double renderDecodeMsTotal = 0.0;
    double renderCompositeMsTotal = 0.0;
    double renderReadbackMsTotal = 0.0;
    double decoderUploadMsTotal = 0.0;
    double vulkanDetectMsTotal = 0.0;
    double ncnnInputMsTotal = 0.0;
    double ncnnExtractMsTotal = 0.0;
    double ncnnExtractLevel8MsTotal = 0.0;
    double ncnnExtractLevel16MsTotal = 0.0;
    double ncnnExtractLevel32MsTotal = 0.0;
    double ncnnPostMsTotal = 0.0;
    double ncnnTotalMsTotal = 0.0;
    QJsonArray frameRows;
    QJsonArray rawDetectionFrames;
    QHash<int, int> rawDetectionFrameIndexByFrame;
    QHash<int, QJsonArray> trackDetectionsByFrame;
    bool printedAppVulkanFailure = false;
    VulkanHarnessContext detectorContext;
    jcut::vulkan_detector::VulkanDetectorFrameHandoff decoderFrameHandoff;
    jcut::vulkan_detector::VulkanZeroCopyFaceDetector zeroCopyDetector;
    jcut::vulkan_detector::VulkanRes10NcnnFaceDetector res10Detector;
    jcut::vulkan_detector::VulkanScrfdNcnnFaceDetector scrfdDetector;
    PreparedDecoderDetectionSlot decoderDirectSlots[2];
    std::deque<int> pendingDecoderDirectSlots;
    int nextDecoderDirectSlot = 0;
    RuntimeTuning tuning{
        options.stride,
        options.maxDetections,
        options.scrfdTargetSize,
        options.maxFacesPerFrame,
        jcut::facestream::normalizeScrfdModelVariantId(options.scrfdModelVariant),
        options.threshold,
        options.nmsIouThreshold,
        options.trackMatchIouThreshold,
        options.newTrackMinConfidence,
        options.primaryFaceOnly,
        options.smallFaceFallback,
        options.scrfdTiled
    };
    PreviewDebugSettings previewDebugSettings;
    QDateTime paramsMtime;
    const QString detectorSettingsPath = detectorSettingsPathForVideo(options.videoPath);
    QDateTime detectorSettingsMtime;
    QFileInfo detectorSettingsInfo(detectorSettingsPath);
    if (applyRuntimeParamsFile(detectorSettingsPath,
                               detectorSettingsInfo,
                               &tuning,
                               &previewDebugSettings,
                               &detectorSettingsMtime) &&
        options.verbose) {
        std::cout << "loaded_detector_settings=\"" << detectorSettingsPath.toStdString() << "\"\n";
    }
    DetectorControlPanel detectorControls;
    bool runtimePaused = false;
    if (livePreviewWindow) {
        livePreviewWindow->setFollowLatest(previewDebugSettings.followLatest);
        livePreviewWindow->setPreviewPlaybackSpeed(previewDebugSettings.playbackSpeed);
        livePreviewWindow->setShowDetections(previewDebugSettings.showDetections);
        livePreviewWindow->setShowTracks(previewDebugSettings.showTracks);
        livePreviewWindow->setShowLabels(previewDebugSettings.showLabels);
        livePreviewWindow->setShowConfirmedTracks(previewDebugSettings.showConfirmedTracks);
        livePreviewWindow->setShowTentativeTracks(previewDebugSettings.showTentativeTracks);
        livePreviewWindow->setShowLostTracks(previewDebugSettings.showLostTracks);
        livePreviewWindow->setDetectionLineThickness(previewDebugSettings.detectionLineThickness);
        livePreviewWindow->setTrackLineThickness(previewDebugSettings.trackLineThickness);
        livePreviewWindow->setOverlayOpacity(previewDebugSettings.overlayOpacity);
        detectorControls = createDetectorControlPanel(&tuning,
                                                      options.detector,
                                                      options.scrfdTargetSize,
                                                      detectorSettingsPath,
                                                      &options.applyClipGrading,
                                                      !options.clipJsonPath.trimmed().isEmpty(),
                                                      livePreviewWindow.get(),
                                                      &runtimePaused);
        detectorControls.window->show();
        QString saveError;
        if (saveRuntimeTuningFile(detectorSettingsPath,
                                  tuning,
                                  options.detector,
                                  options.scrfdTargetSize,
                                  &previewDebugSettings,
                                  &saveError)) {
            detectorSettingsMtime = QFileInfo(detectorSettingsPath).lastModified();
        } else if (!saveError.isEmpty()) {
            std::cerr << saveError.toStdString() << "\n";
        }
        appPtr->processEvents();
    }
    auto currentRoiRect = [&](const QSize& size) -> QRectF {
        return QRectF(tuning.roiX1 * size.width(),
                      tuning.roiY1 * size.height(),
                      (tuning.roiX2 - tuning.roiX1) * size.width(),
                      (tuning.roiY2 - tuning.roiY1) * size.height());
    };
    auto liveTrackCount = [&](const QVector<Track>& tracks) {
        int count = 0;
        for (const Track& track : tracks) {
            if (track.state != jcut::facestream::ContinuityTrackState::Removed) {
                ++count;
            }
        }
        return count;
    };
    auto presentLivePreviewWindow = [&](const render_detail::OffscreenVulkanFrame& frame,
                                        int frameNumber,
                                        const QVector<Track>& previewTracks,
                                        const QVector<Detection>& previewDetections,
                                        const QString& titlePrefix) -> bool {
        if (!livePreviewWindow) {
            return false;
        }
        livePreviewWindow->setWindowTitle(
            QStringLiteral("%1 - frame %2").arg(titlePrefix).arg(frameNumber));
        livePreviewWindow->setStatusText(
            QStringLiteral("%1 | frame %2 | detections %3 | live tracks %4")
                .arg(titlePrefix)
                .arg(frameNumber)
                .arg(previewDetections.size())
                .arg(liveTrackCount(previewTracks)));
        const bool presented = livePreviewWindow->presentFrame(
            frame,
            frameNumber,
            previewTracks,
            previewDetections,
            currentRoiRect(frame.size),
            previewDetections.size());
        return presented;
    };
    auto renderAndPresentLivePreviewWindow = [&](int frameNumber,
                                                 const QVector<Track>& previewTracks,
                                                 const QVector<Detection>& previewDetections,
                                                 const QString& titlePrefix) -> bool {
        if (!livePreviewWindow) {
            return false;
        }
        render_detail::OffscreenVulkanFrame previewFrame;
        QString previewError;
        if (!jcut::facestream::renderFrameToVulkan(&appFrameProvider,
                                                   sourceClip,
                                                   options.videoPath,
                                                   frameNumber,
                                                   frameNumber,
                                                   renderSize,
                                                   &previewFrame,
                                                   nullptr,
                                                   &previewError)) {
            setPreviewStatusText(previewError);
            return false;
        }
        return presentLivePreviewWindow(previewFrame,
                                        frameNumber,
                                        previewTracks,
                                        previewDetections,
                                        titlePrefix);
    };
    auto emitPreview = [&](const QImage& image,
                           int frameNumber,
                           const QVector<Track>& tracks,
                           const QVector<Detection>& detections,
                           const QString& titlePrefix) -> bool {
        if (image.isNull()) {
            return false;
        }
        const bool previewFrameDue =
            !previewPipelinePrimed || ((frameNumber % effectivePreviewStride) == 0);
        const bool needsPreviewFrame =
            previewFrameDue &&
            (previewSocketPtr ||
             (options.writePreviewFiles && previewWritten < options.previewFrames));
        if (!needsPreviewFrame) {
            return false;
        }
        const QImage preview = buildPreview(image, tracks, detections, currentRoiRect(image.size()));
        bool emitted = false;
        if (previewSocketPtr) {
            emitted = sendPreviewFrame(previewSocketPtr, preview) || emitted;
        }
        if (options.writePreviewFiles && previewWritten < options.previewFrames) {
            const QString previewPath = QDir(options.outputDir).filePath(
                QStringLiteral("preview_%1.png").arg(frameNumber, 6, 10, QChar('0')));
            if (preview.save(previewPath)) {
                ++previewWritten;
                emitted = true;
            }
        }
        previewPipelinePrimed = previewPipelinePrimed || emitted;
        return emitted;
    };
    auto requireSynchronizedPreview = [&](int frameNumber,
                                          const editor::FrameHandle& frameHandle,
                                          const QVector<Track>& tracks,
                                          const QVector<Detection>& detections,
                                          const QString& titlePrefix,
                                          const std::function<QImage()>& previewImageProvider) -> bool {
        Q_UNUSED(frameHandle);
        const bool previewFrameDue =
            !previewPipelinePrimed || ((frameNumber % effectivePreviewStride) == 0);
        if (!previewFrameDue) {
            return true;
        }
        if (!previewRequiresSynchronizedDetection) {
            if (livePreviewWindow) {
                renderAndPresentLivePreviewWindow(frameNumber, tracks, detections, titlePrefix);
            }
            if (previewSocketPtr || (options.writePreviewFiles && previewWritten < options.previewFrames)) {
                const QImage previewImage = previewImageProvider();
                if (!previewImage.isNull()) {
                    emitPreview(previewImage, frameNumber, tracks, detections, titlePrefix);
                }
            }
            appPtr->processEvents(QEventLoop::AllEvents, 1);
            return true;
        }
        bool emitted = false;
        if (livePreviewWindow) {
            emitted = renderAndPresentLivePreviewWindow(frameNumber, tracks, detections, titlePrefix) || emitted;
        }
        if (previewSocketPtr || (options.writePreviewFiles && previewWritten < options.previewFrames)) {
            const QImage previewImage = previewImageProvider();
            if (!previewImage.isNull()) {
                emitted = emitPreview(previewImage, frameNumber, tracks, detections, titlePrefix) || emitted;
            }
        }
        if (emitted) {
            previewPipelinePrimed = true;
            appPtr->processEvents(QEventLoop::AllEvents, 1);
            return true;
        }
        setPreviewStatusText(QStringLiteral("Preview sync failed at frame %1").arg(frameNumber));
        appPtr->processEvents();
        disableSynchronizedPreview(QStringLiteral("frame %1 preview upload failed").arg(frameNumber));
        return true;
    };
    auto requireSynchronizedOffscreenPreview = [&](int frameNumber,
                                                   const render_detail::OffscreenVulkanFrame& frame,
                                                   const QVector<Track>& tracks,
                                                   const QVector<Detection>& detections,
                                                   const QString& titlePrefix,
                                                   const std::function<QImage()>& previewImageProvider) -> bool {
        Q_UNUSED(frame);
        const bool previewFrameDue =
            !previewPipelinePrimed || ((frameNumber % effectivePreviewStride) == 0);
        if (!previewFrameDue) {
            return true;
        }
        if (!previewRequiresSynchronizedDetection) {
            if (livePreviewWindow) {
                presentLivePreviewWindow(frame, frameNumber, tracks, detections, titlePrefix);
            }
            if (previewSocketPtr || (options.writePreviewFiles && previewWritten < options.previewFrames)) {
                const QImage previewImage = previewImageProvider();
                if (!previewImage.isNull()) {
                    emitPreview(previewImage, frameNumber, tracks, detections, titlePrefix);
                }
            }
            appPtr->processEvents(QEventLoop::AllEvents, 1);
            return true;
        }
        bool emitted = false;
        if (livePreviewWindow) {
            emitted = presentLivePreviewWindow(frame, frameNumber, tracks, detections, titlePrefix) || emitted;
        }
        if (previewSocketPtr || (options.writePreviewFiles && previewWritten < options.previewFrames)) {
            const QImage previewImage = previewImageProvider();
            if (!previewImage.isNull()) {
                emitted = emitPreview(previewImage, frameNumber, tracks, detections, titlePrefix) || emitted;
            }
        }
        if (emitted) {
            previewPipelinePrimed = true;
            appPtr->processEvents(QEventLoop::AllEvents, 1);
            return true;
        }
        setPreviewStatusText(QStringLiteral("Preview sync failed at frame %1").arg(frameNumber));
        appPtr->processEvents();
        disableSynchronizedPreview(QStringLiteral("frame %1 preview upload failed").arg(frameNumber));
        return true;
    };
    const int totalFrames = static_cast<int>(qMax<int64_t>(1, targetFrames));
    const int finalFrame = qMax(0, options.startFrame + totalFrames - 1);
    const QString faceStreamPath = QDir(options.outputDir).filePath(QStringLiteral("facestream.part"));
    FaceStreamResumeState resume;
    QString resumeError;
    if (!loadFaceStreamResume(faceStreamPath,
                              options.videoPath,
                              backend,
                              options.startFrame,
                              finalFrame,
                              &resume,
                              &resumeError)) {
        const QString stalePath = faceStreamPath + QStringLiteral(".stale");
        QFile::remove(stalePath);
        QFile::rename(faceStreamPath, stalePath);
        if (options.verbose && !resumeError.isEmpty()) {
            std::cerr << "Ignoring previous facestream checkpoint: "
                      << resumeError.toStdString() << "\n";
        }
        resume = FaceStreamResumeState{};
    }
    int resumeStartOffset = 0;
    while (resumeStartOffset < totalFrames &&
           resume.completedFrames.contains(options.startFrame + resumeStartOffset)) {
        ++resumeStartOffset;
    }
    const int previewStartFrame =
        qMin(finalFrame, options.startFrame + qMax(0, resumeStartOffset));

    if (!previewPipelinePrimed) {
        auto tryWarmupPreview = [&]() -> bool {
            const QVector<Track> warmupTracks;
            const QVector<Detection> warmupDetections;
            bool emitted = false;
            if (livePreviewWindow) {
                emitted = renderAndPresentLivePreviewWindow(
                              previewStartFrame,
                              warmupTracks,
                              warmupDetections,
                              QStringLiteral("JCut DNN FaceStream Generator (Preview Warmup)")) ||
                          emitted;
            }
            if (previewSocketPtr || (options.writePreviewFiles && previewWritten < options.previewFrames)) {
                jcut::facestream::VulkanFrameStats warmupStats;
                const QImage warmupImage = jcut::facestream::renderFrameWithVulkan(&appFrameProvider,
                                                                                   sourceClip,
                                                                                   options.videoPath,
                                                                                   previewStartFrame,
                                                                                   previewStartFrame,
                                                                                   renderSize,
                                                                                   &warmupStats);
                if (!warmupImage.isNull() &&
                    emitPreview(warmupImage,
                                previewStartFrame,
                                warmupTracks,
                                warmupDetections,
                                QStringLiteral("JCut DNN FaceStream Generator (Preview Warmup)"))) {
                    emitted = true;
                }
            }
            return emitted;
        };

        if (livePreviewWindow) {
            setPreviewStatusText(QStringLiteral(
                "Waiting for JCut DNN FaceStream preview before detection starts..."));
            appPtr->processEvents();
        }
        QElapsedTimer previewWarmupTimer;
        previewWarmupTimer.start();
        constexpr qint64 kPreviewWarmupStatusIntervalMs = 3000;
        while (!previewPipelinePrimed) {
            if (tryWarmupPreview()) {
                break;
            }
            if (livePreviewWindow) {
                setPreviewStatusText(
                    QStringLiteral("Waiting for preview frame %1 | %2 ms")
                        .arg(previewStartFrame)
                        .arg(previewWarmupTimer.elapsed()));
            }
            if (livePreviewWindow && livePreviewWindow->hasFailed()) {
                const QString failureReason = livePreviewWindow->failureReason().trimmed().isEmpty()
                    ? QStringLiteral("The Dear ImGui preview window failed during warmup.")
                    : livePreviewWindow->failureReason().trimmed();
                setPreviewStatusText(
                    QStringLiteral("Preview warmup failed | %1").arg(failureReason));
                appPtr->processEvents();
                std::cerr << "Preview warmup failed before detection start: "
                          << failureReason.toStdString() << "\n";
                return 2;
            }
            appPtr->processEvents();
            usleep(50 * 1000);
        }
        if (previewPipelinePrimed && livePreviewWindow) {
            setPreviewStatusText(QStringLiteral(
                "Preview ready. Starting FaceStream detection."));
            appPtr->processEvents();
        }
    }

    const auto wallStart = std::chrono::steady_clock::now();
    processed = resume.processed;
    totalDetections = resume.totalDetections;
    appVulkanFramePathFrames = resume.appVulkanFramePathFrames;
    decoderDirectHandoffFrames = resume.decoderDirectHandoffFrames;
    decoderVulkanUploadFallbackFrames = resume.decoderVulkanUploadFallbackFrames;
    hardwareDirectHandoffFrames = resume.hardwareDirectHandoffFrames;
    hardwareInteropProbeSupportedFrames = resume.hardwareInteropProbeSupportedFrames;
    hardwareInteropProbeFailedFrames = resume.hardwareInteropProbeFailedFrames;
    hardwareFrames = resume.hardwareFrames;
    cpuFrames = resume.cpuFrames;
    renderDecodeMsTotal = resume.renderDecodeMsTotal;
    renderCompositeMsTotal = resume.renderCompositeMsTotal;
    renderReadbackMsTotal = resume.renderReadbackMsTotal;
    decoderUploadMsTotal = resume.decoderUploadMsTotal;
    vulkanDetectMsTotal = resume.vulkanDetectMsTotal;
    frameRows = resume.frameRows;
    rawDetectionFrames = resume.rawDetectionFrames;
    trackDetectionsByFrame = resume.trackDetectionsByFrame;
    for (int i = 0; i < rawDetectionFrames.size(); ++i) {
        const int frameNumber = rawDetectionFrames.at(i).toObject().value(QStringLiteral("frame")).toInt(-1);
        if (frameNumber >= 0) {
            rawDetectionFrameIndexByFrame.insert(frameNumber, i);
        }
    }

    QFile faceStreamFile(faceStreamPath);
    const bool faceStreamExists = QFileInfo::exists(faceStreamPath) && QFileInfo(faceStreamPath).size() > 0;
    if (!faceStreamFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        std::cerr << "Failed to open streaming facestream checkpoint: "
                  << faceStreamPath.toStdString() << "\n";
        return 2;
    }
    if (!faceStreamExists) {
        appendBinaryJsonRecord(&faceStreamFile, QJsonObject{
            {QStringLiteral("type"), QStringLiteral("meta")},
            {QStringLiteral("schema"), QStringLiteral("jcut_facestream_part_v1")},
            {QStringLiteral("video"), options.videoPath},
            {QStringLiteral("backend"), backend},
            {QStringLiteral("start_frame"), options.startFrame},
            {QStringLiteral("end_frame"), finalFrame},
            {QStringLiteral("stride"), tuning.stride},
            {QStringLiteral("created_utc_ms"), QDateTime::currentDateTimeUtc().toMSecsSinceEpoch()}
        });
    }
    if (options.verbose && !resume.completedFrames.isEmpty()) {
        std::cout << "resumed_facestream_checkpoint=\""
                  << faceStreamPath.toStdString()
                  << "\" completed_frames=" << resume.completedFrames.size()
                  << "\n";
    }
    int lastProgressPercent = -1;
    std::chrono::steady_clock::time_point lastProgressAt = wallStart;
    AdaptiveEtaTracker etaTracker;
    int latestProcessedFrame = options.startFrame - 1;
    for (int completedFrame : resume.completedFrames) {
        latestProcessedFrame = qMax(latestProcessedFrame, completedFrame);
    }
    syncDetectorPreviewPanel(&detectorControls,
                             livePreviewWindow.get(),
                             options.startFrame,
                             qMax(options.startFrame, latestProcessedFrame));
    QVector<Track> runtimeTracks =
        jcut::facestream::buildContinuityTracksFromDetectionFrames(
            rawDetectionFrames,
            trackingTuningForRuntime(tuning));
    auto detectionsForFrame = [&](int frameNumber) {
        QVector<Detection> frameDetections;
        const int index = rawDetectionFrameIndexByFrame.value(frameNumber, -1);
        if (index < 0 || index >= rawDetectionFrames.size()) {
            return frameDetections;
        }
        const QJsonObject frameObject = rawDetectionFrames.at(index).toObject();
        const QJsonArray detectionRows = frameObject.value(QStringLiteral("detections")).toArray();
        frameDetections.reserve(detectionRows.size());
        for (const QJsonValue& detectionValue : detectionRows) {
            const QJsonObject detectionObject = detectionValue.toObject();
            Detection detection;
            detection.box = QRectF(detectionObject.value(QStringLiteral("x")).toDouble(),
                                   detectionObject.value(QStringLiteral("y")).toDouble(),
                                   detectionObject.value(QStringLiteral("w")).toDouble(),
                                   detectionObject.value(QStringLiteral("h")).toDouble());
            detection.confidence =
                static_cast<float>(detectionObject.value(QStringLiteral("confidence")).toDouble());
            if (detection.box.isValid() && !detection.box.isEmpty()) {
                frameDetections.push_back(detection);
            }
        }
        return frameDetections;
    };
    auto tracksForFrame = [&](int frameNumber) {
        QVector<Track> frameTracks;
        const QJsonArray trackRows = trackDetectionsByFrame.value(frameNumber);
        frameTracks.reserve(trackRows.size());
        for (const QJsonValue& trackValue : trackRows) {
            const QJsonObject trackObject = trackValue.toObject();
            Track track;
            track.id = trackObject.value(QStringLiteral("track_id")).toInt(-1);
            track.box = QRectF(trackObject.value(QStringLiteral("track_box_x")).toDouble(),
                               trackObject.value(QStringLiteral("track_box_y")).toDouble(),
                               trackObject.value(QStringLiteral("track_box_w")).toDouble(),
                               trackObject.value(QStringLiteral("track_box_h")).toDouble());
            track.firstFrame = trackObject.value(QStringLiteral("first_frame")).toInt(frameNumber);
            track.lastFrame = trackObject.value(QStringLiteral("last_frame")).toInt(frameNumber);
            track.hits = trackObject.value(QStringLiteral("hits")).toInt(0);
            track.misses = trackObject.value(QStringLiteral("misses")).toInt(0);
            const QString state = trackObject.value(QStringLiteral("track_state")).toString();
            if (state == QStringLiteral("confirmed")) {
                track.state = jcut::facestream::ContinuityTrackState::Confirmed;
            } else if (state == QStringLiteral("lost")) {
                track.state = jcut::facestream::ContinuityTrackState::Lost;
            } else if (state == QStringLiteral("removed")) {
                track.state = jcut::facestream::ContinuityTrackState::Removed;
            } else {
                track.state = jcut::facestream::ContinuityTrackState::Tentative;
            }
            if (track.id >= 0 && track.box.isValid() && !track.box.isEmpty()) {
                frameTracks.push_back(track);
            }
        }
        return frameTracks;
    };
    auto advanceRuntimeTracks = [&](int frameNumber,
                                    const QVector<Detection>& detections,
                                    const QSize& detectionFrameSize) -> QVector<Track> {
        jcut::facestream::updateContinuityTracks(&runtimeTracks,
                                                 detections,
                                                 frameNumber,
                                                 detectionFrameSize,
                                                 trackingTuningForRuntime(tuning));
        return runtimeTracks;
    };
    auto recordProcessedFrame = [&](int frameOffset,
                                    int frameNumber,
                                    const jcut::facestream::VulkanFrameStats& renderStats,
                                    const QVector<Detection>& detections,
                                    const QVector<Track>& tracks,
                                    const QSize& detectionFrameSize,
                                    bool appVulkanFrame,
                                    bool decoderVulkanUploadFallback,
                                    bool hardwareDirectHandoff,
                                    double decoderUploadMs,
                                    double vulkanDetectMs,
                                    const jcut::vulkan_detector::NcnnInferenceStats& ncnnStats,
                                    const jcut::vulkan_detector::HardwareInteropProbeResult& handoffProbe,
                                    const QString& hardwareDirectAttemptReason) {
        ++processed;
        totalDetections += detections.size();
        renderDecodeMsTotal += renderStats.decodeMs;
        renderCompositeMsTotal += renderStats.compositeMs;
        renderReadbackMsTotal += renderStats.readbackMs;
        decoderUploadMsTotal += decoderUploadMs;
        vulkanDetectMsTotal += vulkanDetectMs;
        ncnnInputMsTotal += ncnnStats.inputMs;
        ncnnExtractMsTotal += ncnnStats.extractMs;
        ncnnExtractLevel8MsTotal += ncnnStats.extractLevel8Ms;
        ncnnExtractLevel16MsTotal += ncnnStats.extractLevel16Ms;
        ncnnExtractLevel32MsTotal += ncnnStats.extractLevel32Ms;
        ncnnPostMsTotal += ncnnStats.postMs;
        ncnnTotalMsTotal += ncnnStats.totalMs;
        const QString detectorId = backend;
        const bool qimageMaterialized = options.materializedGenerateFacestream;
        const QJsonObject rawDetectionFrame =
            buildRawDetectionFrameRecord(frameNumber, detectorId, detectionFrameSize, detections);
        rawDetectionFrames.append(rawDetectionFrame);
        rawDetectionFrameIndexByFrame.insert(frameNumber, rawDetectionFrames.size() - 1);
        const QJsonArray trackDetections =
            jcut::facestream::frameTrackDetections(tracks, frameNumber);
        trackDetectionsByFrame.insert(frameNumber, trackDetections);
        latestProcessedFrame = qMax(latestProcessedFrame, frameNumber);
        const bool decoderDirectHandoff = !appVulkanFrame && hardwareDirectHandoff;
        const QJsonObject frameRow{
            {QStringLiteral("frame"), frameNumber},
            {QStringLiteral("detector"), detectorId},
            {QStringLiteral("detections"), detections.size()},
            {QStringLiteral("tracks"), trackDetections.size()},
            {QStringLiteral("app_vulkan_frame_path"), appVulkanFrame},
            {QStringLiteral("app_render_decode_ms"), renderStats.decodeMs},
            {QStringLiteral("app_render_texture_ms"), renderStats.textureMs},
            {QStringLiteral("app_render_composite_ms"), renderStats.compositeMs},
            {QStringLiteral("app_render_readback_ms"), renderStats.readbackMs},
            {QStringLiteral("vulkan_zero_copy_detection_ms"), vulkanDetectMs},
            {QStringLiteral("decoder_vulkan_upload_ms"), decoderUploadMs},
            {QStringLiteral("ncnn_input_ms"), ncnnStats.inputMs},
            {QStringLiteral("ncnn_extract_ms"), ncnnStats.extractMs},
            {QStringLiteral("ncnn_extract_level8_ms"), ncnnStats.extractLevel8Ms},
            {QStringLiteral("ncnn_extract_level16_ms"), ncnnStats.extractLevel16Ms},
            {QStringLiteral("ncnn_extract_level32_ms"), ncnnStats.extractLevel32Ms},
            {QStringLiteral("ncnn_post_ms"), ncnnStats.postMs},
            {QStringLiteral("ncnn_total_ms"), ncnnStats.totalMs},
            {QStringLiteral("decoder_vulkan_upload_fallback"), decoderVulkanUploadFallback},
            {QStringLiteral("hardware_direct_handoff"), hardwareDirectHandoff},
            {QStringLiteral("hardware_direct_attempt_reason"), hardwareDirectAttemptReason},
            {QStringLiteral("qimage_materialized"), qimageMaterialized}
        };
        frameRows.append(frameRow);
        QJsonObject streamRow = frameRow;
        streamRow.insert(QStringLiteral("type"), QStringLiteral("frame"));
        streamRow.insert(QStringLiteral("schema"), QStringLiteral("jcut_facestream_frame_v1"));
        streamRow.insert(QStringLiteral("video"), options.videoPath);
        streamRow.insert(QStringLiteral("backend"), backend);
        streamRow.insert(QStringLiteral("decoder_direct_handoff"), decoderDirectHandoff);
        streamRow.insert(QStringLiteral("hardware_interop_probe_supported"), handoffProbe.supported);
        streamRow.insert(QStringLiteral("hardware_interop_probe_failed"), !handoffProbe.path.isEmpty() && !handoffProbe.supported);
        streamRow.insert(QStringLiteral("hardware_interop_probe_path"), handoffProbe.path);
        streamRow.insert(QStringLiteral("hardware_interop_probe_reason"), handoffProbe.reason);
        streamRow.insert(QStringLiteral("hardware_frame"), appVulkanFrame || hardwareDirectHandoff);
        streamRow.insert(QStringLiteral("cpu_frame"), qimageMaterialized || decoderVulkanUploadFallback);
        streamRow.insert(QStringLiteral("detection_boxes"),
                         rawDetectionFrame.value(QStringLiteral("detections")).toArray());
        streamRow.insert(QStringLiteral("track_detections"), trackDetections);
        if (!appendBinaryJsonRecord(&faceStreamFile, streamRow)) {
            std::cerr << "Failed to append streaming facestream checkpoint: "
                      << faceStreamPath.toStdString() << "\n";
            return false;
        }
        resume.completedFrames.insert(frameNumber);
        const bool logThisFrame =
            options.verbose ||
            (options.logInterval > 0 && (processed % options.logInterval) == 0);
        if (logThisFrame) {
            std::cout << "frame=" << frameNumber
                      << " detector=" << detectorId.toStdString()
                      << " detections=" << detections.size()
                      << " app_vulkan_frame_path=" << (appVulkanFrame ? 1 : 0)
                      << " render_decode_ms=" << renderStats.decodeMs
                      << " render_texture_ms=" << renderStats.textureMs
                      << " render_composite_ms=" << renderStats.compositeMs
                      << " render_readback_ms=" << renderStats.readbackMs
                      << " vulkan_zero_copy_detection_ms=" << vulkanDetectMs
                      << " decoder_vulkan_upload_ms=" << decoderUploadMs
                      << " ncnn_input_ms=" << ncnnStats.inputMs
                      << " ncnn_extract_ms=" << ncnnStats.extractMs
                      << " ncnn_extract_level8_ms=" << ncnnStats.extractLevel8Ms
                      << " ncnn_extract_level16_ms=" << ncnnStats.extractLevel16Ms
                      << " ncnn_extract_level32_ms=" << ncnnStats.extractLevel32Ms
                      << " ncnn_post_ms=" << ncnnStats.postMs
                      << " ncnn_total_ms=" << ncnnStats.totalMs
                      << " decoder_vulkan_upload_fallback=" << (decoderVulkanUploadFallback ? 1 : 0)
                      << " hardware_direct_handoff=" << (hardwareDirectHandoff ? 1 : 0)
                      << " hardware_direct_attempt_reason=\"" << hardwareDirectAttemptReason.toStdString() << "\""
                      << " qimage_materialized=" << (qimageMaterialized ? 1 : 0)
                      << "\n";
        }
        if (options.progress && !logThisFrame &&
            shouldRenderProgress(frameOffset,
                                 totalFrames,
                                 processed,
                                 &lastProgressPercent,
                                 &lastProgressAt)) {
            const auto now = std::chrono::steady_clock::now();
            const double elapsedSec = std::chrono::duration<double>(now - wallStart).count();
            renderProgressLine(frameOffset,
                               totalFrames,
                               frameNumber,
                               processed,
                               totalDetections,
                               detections.size(),
                               elapsedSec,
                               processed > 0 ? vulkanDetectMsTotal / processed : 0.0,
                               &etaTracker);
        }
        if (detectorControls.window) {
            const auto now = std::chrono::steady_clock::now();
            const double elapsedSec = std::chrono::duration<double>(now - wallStart).count();
            updateDetectorRuntimeStats(&detectorControls,
                                       frameOffset,
                                       totalFrames,
                                       frameNumber,
                                       processed,
                                       totalDetections,
                                       liveTrackCount(tracks),
                                       elapsedSec,
                                       &etaTracker);
        }
        return true;
    };
    const auto emptyProbe = jcut::vulkan_detector::HardwareInteropProbeResult{};
    const auto emptyNcnnStats = jcut::vulkan_detector::NcnnInferenceStats{};
    const auto decoderDirectPipelineEnabled = [&]() {
        return preferDecoderDirectDetection &&
               zeroCopyVulkanDetector &&
               !options.heuristicZeroCopyDetector &&
               (!options.scrfdDetector || !tuning.scrfdTiled);
    };
    auto finalizePreparedDecoderSlot = [&](int slotIndex) -> bool {
        PreparedDecoderDetectionSlot& slot = decoderDirectSlots[slotIndex];
        if (!slot.active) {
            return true;
        }
        QVector<Detection> detections;
        double vulkanDetectMs = 0.0;
        jcut::vulkan_detector::NcnnInferenceStats ncnnStats;
        error.clear();
        if (options.scrfdDetector) {
            detections = finalizePreparedScrfdDecoderFrame(&slot.preprocessor,
                                                           &scrfdDetector,
                                                           &slot.context,
                                                           &slot.handoff,
                                                           slot.scrfdLayout,
                                                           slot.detectionFrameSize,
                                                           tuning.threshold,
                                                           &vulkanDetectMs,
                                                           &error);
            ncnnStats = scrfdDetector.lastInferenceStats();
        } else {
            detections = finalizePreparedRes10DecoderFrame(&slot.preprocessor,
                                                           &res10Detector,
                                                           &slot.context,
                                                           &slot.handoff,
                                                           slot.detectionFrameSize,
                                                           tuning.threshold,
                                                           &vulkanDetectMs,
                                                           &error);
            ncnnStats = res10Detector.lastInferenceStats();
        }
        if (slot.decoderVulkanUploadFallback) {
            ++decoderVulkanUploadFallbackFrames;
        }
        if (!slot.hardwareDirectHandoff && !options.allowCpuUploadFallback) {
            const QString reason = slot.handoff.lastHardwareDirectAttemptReason().isEmpty()
                ? (error.isEmpty()
                       ? QStringLiteral("No supported hardware-direct decoder-to-Vulkan handoff path is available.")
                       : error)
                : slot.handoff.lastHardwareDirectAttemptReason();
            lastHardwareDirectAttemptReason = reason;
            std::cerr << "CPU upload fallback requires explicit approval via --allow-cpu-upload-fallback. "
                      << "Hardware-direct reason: "
                      << reason.toStdString()
                      << "\n";
            slot.active = false;
            return false;
        }
        if (slot.hardwareDirectHandoff) {
            ++hardwareDirectHandoffFrames;
        }
        if (options.requireHardwareVulkanFramePath && !slot.hardwareDirectHandoff) {
            std::cerr << "Hardware Vulkan frame path required, but hardware-direct handoff was not achieved at frame "
                      << slot.frameNumber << ". Reason: "
                      << slot.handoff.lastHardwareDirectAttemptReason().toStdString()
                      << "\n";
            slot.active = false;
            return false;
        }
        if (!error.isEmpty() && detections.isEmpty()) {
            lastHardwareDirectAttemptReason = slot.handoff.lastHardwareDirectAttemptReason();
            std::cerr << "Decoder Vulkan handoff detection failed at frame "
                      << slot.frameNumber << ": " << error.toStdString() << "\n";
            slot.active = false;
            return false;
        }
        lastHardwareDirectAttemptReason = slot.handoff.lastHardwareDirectAttemptReason();
        const QVector<Detection> rawDetections = detections;
        DetectionSanitizeStats sanitizeStats;
        detections = sanitizeDetections(
            detections,
            slot.detectionFrameSize,
            tuning.maxFacesPerFrame,
            tuning,
            &sanitizeStats);
        if (detections.isEmpty()) {
            logDetectionSanitizeStats(
                slot.frameNumber,
                slot.detectionFrameSize,
                tuning,
                sanitizeStats,
                rawDetections);
        }
        const QVector<Track> detectionPreviewTracks =
            advanceRuntimeTracks(slot.frameNumber, detections, slot.detectionFrameSize);
        if (!requireSynchronizedPreview(
                slot.frameNumber,
                slot.decodedFrame,
                detectionPreviewTracks,
                detections,
                QStringLiteral("JCut DNN FaceStream Generator (Decoder Vulkan Upload Fallback)"),
                [&]() -> QImage {
                    return jcut::facestream::renderFrameWithVulkan(&appFrameProvider,
                                                                   sourceClip,
                                                                   options.videoPath,
                                                                   slot.frameNumber,
                                                                   slot.frameNumber,
                                                                   renderSize,
                                                                   nullptr);
                })) {
            slot.active = false;
            return false;
        }
        ++decoderDirectHandoffFrames;
        const jcut::facestream::VulkanFrameStats emptyRenderStats;
        const bool recorded = recordProcessedFrame(slot.frameOffset,
                                                   slot.frameNumber,
                                                   emptyRenderStats,
                                                   detections,
                                                   detectionPreviewTracks,
                                                   slot.detectionFrameSize,
                                                   false,
                                                   slot.decoderVulkanUploadFallback,
                                                   slot.hardwareDirectHandoff,
                                                   slot.decoderUploadMs,
                                                   vulkanDetectMs,
                                                   ncnnStats,
                                                   slot.handoff.lastProbe(),
                                                   slot.handoff.lastHardwareDirectAttemptReason());
        slot.active = false;
        return recorded;
    };
    auto prepareDecoderDirectSlot = [&](int frameOffset, int frameNumber) -> bool {
        PreparedDecoderDetectionSlot& slot = decoderDirectSlots[nextDecoderDirectSlot];
        if (slot.active) {
            std::cerr << "Decoder direct pipeline slot exhaustion at frame "
                      << frameNumber << ".\n";
            return false;
        }
        slot.decodedFrame = decoder.decodeFrame(frameNumber);
        if (slot.decodedFrame.isNull()) {
            return false;
        }
        if (slot.decodedFrame.hasHardwareFrame() || slot.decodedFrame.hasGpuTexture()) {
            ++hardwareFrames;
        }
        if (slot.decodedFrame.hasCpuImage()) {
            ++cpuFrames;
        }
        if (detectorContext.device == VK_NULL_HANDLE) {
            if (!detectorContext.initialize(&error)) {
                std::cerr << "Failed to initialize shared Vulkan detector context for decoder handoff at frame "
                          << frameNumber << ": " << error.toStdString() << "\n";
                return false;
            }
        }
        if (!slot.context.attachExternalDevice(detectorContext.detectorContext(), &error)) {
            std::cerr << "Failed to attach decoder pipeline slot to shared Vulkan detector context at frame "
                      << frameNumber << ": " << error.toStdString() << "\n";
            return false;
        }
        if (!slot.handoff.isInitialized()) {
            if (!slot.handoff.initialize(slot.context.detectorContext(), &error)) {
                std::cerr << "Failed to initialize decoder frame handoff module at frame "
                          << frameNumber << ": " << error.toStdString() << "\n";
                return false;
            }
        }
        if (slot.decodedFrame.hasHardwareFrame()) {
            const auto probe = slot.handoff.probeHardwareInterop(slot.decodedFrame);
            lastHardwareInteropProbeReason = probe.reason;
            lastHardwareInteropProbePath = probe.path;
            if (probe.supported) {
                ++hardwareInteropProbeSupportedFrames;
            } else {
                ++hardwareInteropProbeFailedFrames;
            }
            if (options.requireHardwareVulkanFramePath && !probe.supported) {
                std::cerr << "Hardware direct Vulkan handoff probe failed at frame "
                          << frameNumber << ": " << probe.reason.toStdString() << "\n";
                return false;
            }
        }
        slot.frameNumber = frameNumber;
        slot.frameOffset = frameOffset;
        slot.detectionFrameSize = slot.decodedFrame.size().isValid() ? slot.decodedFrame.size() : renderSize;
        slot.decoderUploadMs = 0.0;
        slot.hardwareDirectHandoff = false;
        slot.decoderVulkanUploadFallback = false;
        slot.scrfdLayout = {};
        error.clear();
        bool prepared = false;
        if (options.scrfdDetector) {
            prepared = prepareScrfdDecoderFrame(&slot.preprocessor,
                                                &scrfdDetector,
                                                &slot.context,
                                                &slot.handoff,
                                                slot.decodedFrame,
                                                scrfdParamPath,
                                                scrfdBinPath,
                                                tuning.scrfdTargetSize,
                                                !options.verbose,
                                                options.allowCpuUploadFallback,
                                                &slot.scrfdLayout,
                                                &slot.decoderUploadMs,
                                                &slot.hardwareDirectHandoff,
                                                &slot.detectionFrameSize,
                                                &error);
        } else {
            prepared = prepareRes10DecoderFrame(&slot.preprocessor,
                                                &res10Detector,
                                                &slot.context,
                                                &slot.handoff,
                                                slot.decodedFrame,
                                                res10ParamPath,
                                                res10BinPath,
                                                !options.verbose,
                                                options.allowCpuUploadFallback,
                                                &slot.decoderUploadMs,
                                                &slot.hardwareDirectHandoff,
                                                &slot.detectionFrameSize,
                                                &error);
        }
        if (!prepared) {
            if (!error.isEmpty()) {
                std::cerr << "Decoder Vulkan handoff preparation failed at frame "
                          << frameNumber << ": " << error.toStdString() << "\n";
            }
            return false;
        }
        slot.decoderVulkanUploadFallback = slot.handoff.usedCpuUpload();
        slot.active = true;
        pendingDecoderDirectSlots.push_back(nextDecoderDirectSlot);
        nextDecoderDirectSlot = (nextDecoderDirectSlot + 1) % 2;
        return true;
    };
    for (int frameOffset = resumeStartOffset; frameOffset < totalFrames; ++frameOffset) {
        const int frameNumber = options.startFrame + frameOffset;
        if (resume.completedFrames.contains(frameNumber)) {
            ++decoded;
            const bool previewFrameDue =
                !previewPipelinePrimed || ((frameNumber % effectivePreviewStride) == 0);
            if (livePreviewWindow && previewFrameDue) {
                const QVector<Track>& resumedTracks = runtimeTracks;
                const QVector<Detection> resumedDetections;
                if (preferDecoderDirectDetection) {
                    const editor::FrameHandle resumedDecodedFrame = decoder.decodeFrame(frameNumber);
                    if (resumedDecodedFrame.isNull() ||
                        !requireSynchronizedPreview(
                            frameNumber,
                            resumedDecodedFrame,
                            resumedTracks,
                            resumedDetections,
                            QStringLiteral("JCut DNN FaceStream Generator (Resume)"),
                            [&]() -> QImage {
                                const editor::FrameHandle imageFrame = decoder.decodeFrame(frameNumber);
                                return imageFrame.hasCpuImage() ? imageFrame.cpuImage() : QImage();
                            })) {
                        std::cerr << "Preview synchronization failed while advancing resumed frame "
                                  << frameNumber
                                  << ".\n";
                        return 2;
                    }
                } else {
                    render_detail::OffscreenVulkanFrame resumedFrame;
                    jcut::facestream::VulkanFrameStats resumedStats;
                    QString resumedError;
                    if (!jcut::facestream::renderFrameToVulkan(&appFrameProvider,
                                                               sourceClip,
                                                               options.videoPath,
                                                               frameNumber,
                                                               frameNumber,
                                                               renderSize,
                                                               &resumedFrame,
                                                               &resumedStats,
                                                               &resumedError) ||
                        !requireSynchronizedOffscreenPreview(
                            frameNumber,
                            resumedFrame,
                            resumedTracks,
                            resumedDetections,
                            QStringLiteral("JCut DNN FaceStream Generator (Resume)"),
                            [&]() -> QImage {
                                return jcut::facestream::renderFrameWithVulkan(&appFrameProvider,
                                                                               sourceClip,
                                                                               options.videoPath,
                                                                               frameNumber,
                                                                               frameNumber,
                                                                               renderSize,
                                                                               nullptr);
                            })) {
                        std::cerr << "Preview synchronization failed while advancing resumed frame "
                                  << frameNumber
                                  << ". "
                                  << resumedError.toStdString()
                                  << "\n";
                        return 2;
                    }
                }
            }
            if (detectorControls.window) {
                const auto now = std::chrono::steady_clock::now();
                const double elapsedSec = std::chrono::duration<double>(now - wallStart).count();
                updateDetectorRuntimeStats(&detectorControls,
                                           frameOffset,
                                           totalFrames,
                                           frameNumber,
                                           processed,
                                           totalDetections,
                                           0,
                                           elapsedSec,
                                           &etaTracker);
            }
            if (options.progress &&
                shouldRenderProgress(frameOffset,
                                     totalFrames,
                                     processed,
                                     &lastProgressPercent,
                                     &lastProgressAt)) {
                const auto now = std::chrono::steady_clock::now();
                const double elapsedSec = std::chrono::duration<double>(now - wallStart).count();
                renderProgressLine(frameOffset,
                                   totalFrames,
                                   frameNumber,
                                   processed,
                                   totalDetections,
                                   0,
                                   elapsedSec,
                                   processed > 0 ? vulkanDetectMsTotal / processed : 0.0,
                                   &etaTracker);
            }
            continue;
        }
        if (livePreviewWindow || detectorControls.window) {
            appPtr->processEvents();
            if (detectorControls.window && !detectorControls.window->isVisible()) {
                detectorControls.window = nullptr;
            }
            if (livePreviewWindow) {
                livePreviewWindow->setTimelineRange(options.startFrame,
                                                   finalFrame,
                                                   qMax(options.startFrame, latestProcessedFrame));
                livePreviewWindow->setProcessingPaused(runtimePaused);
                syncDetectorPreviewPanel(&detectorControls,
                                         livePreviewWindow.get(),
                                         options.startFrame,
                                         qMax(options.startFrame, latestProcessedFrame));
                livePreviewWindow->pumpEvents();
                runtimePaused = livePreviewWindow->processingPausedRequested();
            }
        }
        while (runtimePaused) {
            if (livePreviewWindow) {
                setPreviewStatusText(QStringLiteral(
                    "FaceStream detection paused. Resume from the runtime controls to continue."));
                livePreviewWindow->setTimelineRange(options.startFrame,
                                                   finalFrame,
                                                   qMax(options.startFrame, latestProcessedFrame));
                livePreviewWindow->setProcessingPaused(true);
                syncDetectorPreviewPanel(&detectorControls,
                                         livePreviewWindow.get(),
                                         options.startFrame,
                                         qMax(options.startFrame, latestProcessedFrame));
            }
            appPtr->processEvents(QEventLoop::AllEvents, 16);
            if (livePreviewWindow) {
                livePreviewWindow->pumpEvents();
                syncDetectorPreviewPanel(&detectorControls,
                                         livePreviewWindow.get(),
                                         options.startFrame,
                                         qMax(options.startFrame, latestProcessedFrame));
                runtimePaused = livePreviewWindow->processingPausedRequested();
                const int previewFrame = livePreviewWindow->requestedPreviewFrame();
                if (previewFrame >= options.startFrame && previewFrame <= latestProcessedFrame &&
                    (livePreviewWindow->lastPresentedSourceFrame() != previewFrame ||
                     livePreviewWindow->previewRefreshRequested())) {
                    renderAndPresentLivePreviewWindow(previewFrame,
                                                     tracksForFrame(previewFrame),
                                                     detectionsForFrame(previewFrame),
                                                     QStringLiteral("JCut DNN FaceStream Generator (History Preview)"));
                }
            }
            if (detectorControls.window && !detectorControls.window->isVisible()) {
                detectorControls.window = nullptr;
                runtimePaused = false;
                break;
            }
            usleep(16 * 1000);
        }
        syncSourceClipGrading();
        if (!options.paramsFile.isEmpty()) {
            QFileInfo paramsInfo(options.paramsFile);
            if (applyRuntimeParamsFile(options.paramsFile, paramsInfo, &tuning, nullptr, &paramsMtime)) {
                tuning.scrfdModelVariant = options.scrfdModelVariant;
                syncDetectorControlPanel(&detectorControls, tuning);
                if (options.verbose) {
                    std::cout << "runtime_params"
                              << " scrfd_model_variant=" << tuning.scrfdModelVariant.toStdString()
                              << " stride=" << tuning.stride
                              << " threshold=" << tuning.threshold
                              << " max_detections=" << tuning.maxDetections
                              << " max_faces_per_frame=" << tuning.maxFacesPerFrame
                              << " nms_iou_threshold=" << tuning.nmsIouThreshold
                              << " track_match_iou_threshold=" << tuning.trackMatchIouThreshold
                              << " new_track_min_confidence=" << tuning.newTrackMinConfidence
                              << " primary_face_only=" << (tuning.primaryFaceOnly ? 1 : 0)
                              << " small_face_fallback=" << (tuning.smallFaceFallback ? 1 : 0)
                              << "\n";
                }
            }
        }
        QFileInfo liveSettingsInfo(detectorSettingsPath);
        if (applyRuntimeParamsFile(detectorSettingsPath,
                                   liveSettingsInfo,
                                   &tuning,
                                   &previewDebugSettings,
                                   &detectorSettingsMtime)) {
            tuning.scrfdModelVariant = options.scrfdModelVariant;
            syncDetectorControlPanel(&detectorControls, tuning);
            if (livePreviewWindow) {
                livePreviewWindow->setFollowLatest(previewDebugSettings.followLatest);
                livePreviewWindow->setPreviewPlaybackSpeed(previewDebugSettings.playbackSpeed);
                livePreviewWindow->setShowDetections(previewDebugSettings.showDetections);
                livePreviewWindow->setShowTracks(previewDebugSettings.showTracks);
                livePreviewWindow->setShowLabels(previewDebugSettings.showLabels);
                livePreviewWindow->setShowConfirmedTracks(previewDebugSettings.showConfirmedTracks);
                livePreviewWindow->setShowTentativeTracks(previewDebugSettings.showTentativeTracks);
                livePreviewWindow->setShowLostTracks(previewDebugSettings.showLostTracks);
                livePreviewWindow->setDetectionLineThickness(previewDebugSettings.detectionLineThickness);
                livePreviewWindow->setTrackLineThickness(previewDebugSettings.trackLineThickness);
                livePreviewWindow->setOverlayOpacity(previewDebugSettings.overlayOpacity);
            }
            if (options.verbose) {
                std::cout << "detector_settings"
                          << " scrfd_model_variant=" << tuning.scrfdModelVariant.toStdString()
                          << " threshold=" << tuning.threshold
                          << " max_faces_per_frame=" << tuning.maxFacesPerFrame
                          << " primary_face_only=" << (tuning.primaryFaceOnly ? 1 : 0)
                          << "\n";
            }
        }
        ++decoded;
        if ((frameNumber % tuning.stride) != 0) {
            if (detectorControls.window) {
                const auto now = std::chrono::steady_clock::now();
                const double elapsedSec = std::chrono::duration<double>(now - wallStart).count();
                updateDetectorRuntimeStats(&detectorControls,
                                           frameOffset,
                                           totalFrames,
                                           frameNumber,
                                           processed,
                                           totalDetections,
                                           0,
                                           elapsedSec,
                                           &etaTracker);
            }
            continue;
        }

        jcut::facestream::VulkanFrameStats renderStats;
        QVector<Detection> detections;
        QSize detectionFrameSize = renderSize;
        bool appVulkanFrame = false;
        bool decoderVulkanUploadFallback = false;
        bool hardwareDirectHandoff = false;
        double vulkanDetectMs = 0.0;
        double decoderUploadMs = 0.0;

        if (zeroCopyVulkanDetector) {
            auto processDecoderFrameDirectly = [&]() -> bool {
                editor::FrameHandle decodedFrame = decoder.decodeFrame(frameNumber);
                if (decodedFrame.isNull()) {
                    return false;
                }
                if (decodedFrame.hasHardwareFrame() || decodedFrame.hasGpuTexture()) {
                    ++hardwareFrames;
                }
                if (decodedFrame.hasCpuImage()) {
                    ++cpuFrames;
                }
                if (detectorContext.device == VK_NULL_HANDLE) {
                    if (!detectorContext.initialize(&error)) {
                        std::cerr << "Failed to initialize Vulkan detector context for decoder handoff at frame "
                                  << frameNumber << ": " << error.toStdString() << "\n";
                        return false;
                    }
                }
                if (!decoderFrameHandoff.isInitialized()) {
                    if (!decoderFrameHandoff.initialize(detectorContext.detectorContext(), &error)) {
                        std::cerr << "Failed to initialize decoder frame handoff module at frame "
                                  << frameNumber << ": " << error.toStdString() << "\n";
                        return false;
                    }
                }
                if (decodedFrame.hasHardwareFrame()) {
                    const auto probe = decoderFrameHandoff.probeHardwareInterop(decodedFrame);
                    lastHardwareInteropProbeReason = probe.reason;
                    lastHardwareInteropProbePath = probe.path;
                    if (probe.supported) {
                        ++hardwareInteropProbeSupportedFrames;
                    } else {
                        ++hardwareInteropProbeFailedFrames;
                    }
                    if (options.requireHardwareVulkanFramePath && !probe.supported) {
                        std::cerr << "Hardware direct Vulkan handoff probe failed at frame "
                                  << frameNumber << ": " << probe.reason.toStdString() << "\n";
                        return false;
                    }
                }
                detectionFrameSize = decodedFrame.size().isValid() ? decodedFrame.size() : renderSize;
                error.clear();
                if (options.heuristicZeroCopyDetector) {
                    error = QStringLiteral(
                        "Decoder fallback handoff is implemented for the Res10 Vulkan path only.");
                    detections.clear();
                } else if (options.scrfdDetector) {
                    detections = detectScrfdFromDecoderFrame(&zeroCopyDetector,
                                                             &scrfdDetector,
                                                             &detectorContext,
                                                             &decoderFrameHandoff,
                                                             decodedFrame,
                                                             scrfdParamPath,
                                                             scrfdBinPath,
                                                             tuning.threshold,
                                                             tuning.scrfdTargetSize,
                                                             tuning.scrfdTiled,
                                                             !options.verbose,
                                                             options.allowCpuUploadFallback,
                                                             &decoderUploadMs,
                                                             &vulkanDetectMs,
                                                             &hardwareDirectHandoff,
                                                             &error);
                } else {
                    detections = detectRes10FromDecoderFrame(&zeroCopyDetector,
                                                             &res10Detector,
                                                             &detectorContext,
                                                             &decoderFrameHandoff,
                                                             decodedFrame,
                                                             res10ParamPath,
                                                             res10BinPath,
                                                             tuning.threshold,
                                                             options.allowCpuUploadFallback,
                                                             &decoderUploadMs,
                                                             &vulkanDetectMs,
                                                             &hardwareDirectHandoff,
                                                             &error);
                }
                decoderVulkanUploadFallback = decoderFrameHandoff.usedCpuUpload();
                if (decoderVulkanUploadFallback) {
                    ++decoderVulkanUploadFallbackFrames;
                }
                if (!hardwareDirectHandoff && !options.allowCpuUploadFallback) {
                    const QString reason = decoderFrameHandoff.lastHardwareDirectAttemptReason().isEmpty()
                        ? (error.isEmpty()
                               ? QStringLiteral("No supported hardware-direct decoder-to-Vulkan handoff path is available.")
                               : error)
                        : decoderFrameHandoff.lastHardwareDirectAttemptReason();
                    lastHardwareDirectAttemptReason = reason;
                    std::cerr << "CPU upload fallback requires explicit approval via --allow-cpu-upload-fallback. "
                              << "Hardware-direct reason: "
                              << reason.toStdString()
                              << "\n";
                    return false;
                }
                decoderUploadMsTotal += decoderUploadMs;
                if (hardwareDirectHandoff) {
                    ++hardwareDirectHandoffFrames;
                }
                if (options.requireHardwareVulkanFramePath && !hardwareDirectHandoff) {
                    std::cerr << "Hardware Vulkan frame path required, but hardware-direct handoff was not achieved at frame "
                              << frameNumber << ". Reason: "
                              << decoderFrameHandoff.lastHardwareDirectAttemptReason().toStdString()
                              << "\n";
                    return false;
                }
                if (!error.isEmpty() && detections.isEmpty()) {
                    lastHardwareDirectAttemptReason = decoderFrameHandoff.lastHardwareDirectAttemptReason();
                    std::cerr << "Decoder Vulkan handoff detection failed at frame "
                              << frameNumber << ": " << error.toStdString() << "\n";
                    return false;
                }
                lastHardwareDirectAttemptReason = decoderFrameHandoff.lastHardwareDirectAttemptReason();
                const QVector<Detection> rawDetections = detections;
                DetectionSanitizeStats sanitizeStats;
                detections = sanitizeDetections(
                    detections,
                    detectionFrameSize,
                    tuning.maxFacesPerFrame,
                    tuning,
                    &sanitizeStats);
                if (detections.isEmpty()) {
                    logDetectionSanitizeStats(
                        frameNumber,
                        detectionFrameSize,
                        tuning,
                        sanitizeStats,
                        rawDetections);
                }
                const QVector<Track> detectionPreviewTracks =
                    advanceRuntimeTracks(frameNumber, detections, detectionFrameSize);
                if (!requireSynchronizedPreview(
                        frameNumber,
                        decodedFrame,
                        detectionPreviewTracks,
                        detections,
                        QStringLiteral("JCut DNN FaceStream Generator (Decoder Vulkan Upload Fallback)"),
                        [&]() -> QImage {
                            return jcut::facestream::renderFrameWithVulkan(&appFrameProvider,
                                                                           sourceClip,
                                                                           options.videoPath,
                                                                           frameNumber,
                                                                           frameNumber,
                                                                           renderSize,
                                                                           nullptr);
                        })) {
                    return false;
                }
                return true;
            };

            render_detail::OffscreenVulkanFrame vulkanFrame;
            error.clear();
            bool frameProcessed = false;
            if (preferDecoderDirectDetection) {
                if (decoderDirectPipelineEnabled()) {
                    if (!prepareDecoderDirectSlot(frameOffset, frameNumber)) {
                        continue;
                    }
                    if (pendingDecoderDirectSlots.size() < 2) {
                        if (detectorControls.window) {
                            const auto now = std::chrono::steady_clock::now();
                            const double elapsedSec = std::chrono::duration<double>(now - wallStart).count();
                            updateDetectorRuntimeStats(&detectorControls,
                                                       frameOffset,
                                                       totalFrames,
                                                       frameNumber,
                                                       processed,
                                                       totalDetections,
                                                       0,
                                                       elapsedSec,
                                                       &etaTracker);
                        }
                        continue;
                    }
                    const int slotIndex = pendingDecoderDirectSlots.front();
                    pendingDecoderDirectSlots.pop_front();
                    if (!finalizePreparedDecoderSlot(slotIndex)) {
                        continue;
                    }
                    continue;
                }
                frameProcessed = processDecoderFrameDirectly();
                if (frameProcessed) {
                    ++decoderDirectHandoffFrames;
                } else {
                    continue;
                }
            } else if (!([&]() {
                             const bool synchronizedPreviewFrameNeeded =
                                 previewRequiresSynchronizedDetection &&
                                 (!previewPipelinePrimed || ((frameNumber % effectivePreviewStride) == 0));
                             Q_UNUSED(synchronizedPreviewFrameNeeded);
                             return jcut::facestream::renderFrameToVulkan(&appFrameProvider,
                                                                          sourceClip,
                                                                          options.videoPath,
                                                                          frameNumber,
                                                                          frameNumber,
                                                                          renderSize,
                                                                          &vulkanFrame,
                                                                          &renderStats,
                                                                          &error);
                         })()) {
                if (!printedAppVulkanFailure) {
                    std::cerr << "Application Vulkan frame path unavailable: "
                              << error.toStdString() << "\n";
                    printedAppVulkanFailure = true;
                }
                return 2;
            } else {
                appVulkanFrame = true;
                ++appVulkanFramePathFrames;
                ++hardwareFrames;
                detectionFrameSize = vulkanFrame.size;
                error.clear();
                if (options.heuristicZeroCopyDetector) {
                    detections = detectVulkanFrame(&zeroCopyDetector,
                                                   &detectorContext,
                                                   vulkanFrame,
                                                   tuning.maxDetections,
                                                   tuning.threshold,
                                                   &vulkanDetectMs,
                                                   &error);
                } else if (options.scrfdDetector) {
                    detections = detectScrfdVulkanFrame(&zeroCopyDetector,
                                                        &scrfdDetector,
                                                        &detectorContext,
                                                        vulkanFrame,
                                                        scrfdParamPath,
                                                        scrfdBinPath,
                                                        tuning.threshold,
                                                        tuning.scrfdTargetSize,
                                                        tuning.scrfdTiled,
                                                        !options.verbose,
                                                        &vulkanDetectMs,
                                                        &error);
                } else {
                    detections = detectRes10VulkanFrame(&zeroCopyDetector,
                                                        &res10Detector,
                                                        &detectorContext,
                                                        vulkanFrame,
                                                        res10ParamPath,
                                                        res10BinPath,
                                                        tuning.threshold,
                                                        tuning.smallFaceFallback,
                                                        !options.verbose,
                                                        &vulkanDetectMs,
                                                        &error);
                }
                if (!error.isEmpty() && detections.isEmpty()) {
                    std::cerr << "Vulkan zero-copy detection failed at frame "
                              << frameNumber << ": " << error.toStdString() << "\n";
                    return 2;
                }
                const QVector<Detection> rawDetections = detections;
                DetectionSanitizeStats sanitizeStats;
                detections = sanitizeDetections(
                    detections,
                    detectionFrameSize,
                    tuning.maxFacesPerFrame,
                    tuning,
                    &sanitizeStats);
                if (detections.isEmpty()) {
                    logDetectionSanitizeStats(
                        frameNumber,
                        detectionFrameSize,
                        tuning,
                        sanitizeStats,
                        rawDetections);
                }
                const QVector<Track> detectionPreviewTracks =
                    advanceRuntimeTracks(frameNumber, detections, detectionFrameSize);
                const bool previewFrameDue =
                    !previewPipelinePrimed || ((frameNumber % effectivePreviewStride) == 0);
                const bool needsPreviewFrame =
                    previewFrameDue &&
                    (livePreviewWindow ||
                     previewSocketPtr ||
                     (options.writePreviewFiles && previewWritten < options.previewFrames));
                if (needsPreviewFrame) {
                    if (!requireSynchronizedOffscreenPreview(
                            frameNumber,
                            vulkanFrame,
                            detectionPreviewTracks,
                            detections,
                            QStringLiteral("JCut DNN FaceStream Generator"),
                            [&]() -> QImage {
                                return jcut::facestream::renderFrameWithVulkan(&appFrameProvider,
                                                                               sourceClip,
                                                                               options.videoPath,
                                                                               frameNumber,
                                                                               frameNumber,
                                                                               renderSize,
                                                                               nullptr);
                            })) {
                        return 2;
                    }
                }
            }
        } else {
#if JCUT_HAVE_OPENCV
            render_detail::OffscreenVulkanFrame previewFrame;
            QImage frameImage;
            QString materializedPreviewError;
            const bool materializedRenderOk =
                jcut::facestream::renderFrameToVulkanWithPreviewImage(&appFrameProvider,
                                                                      sourceClip,
                                                                      options.videoPath,
                                                                      frameNumber,
                                                                      frameNumber,
                                                                      renderSize,
                                                                      &previewFrame,
                                                                      &frameImage,
                                                                      &renderStats,
                                                                      &materializedPreviewError);
            appVulkanFrame = materializedRenderOk && previewFrame.valid && !frameImage.isNull();
            if (appVulkanFrame) {
                ++hardwareFrames;
            } else {
                if (appFrameProvider.failed && !printedAppVulkanFailure) {
                    std::cerr << "Application Vulkan frame path unavailable: "
                              << appFrameProvider.failureReason.toStdString() << "\n";
                    printedAppVulkanFailure = true;
                } else if (!materializedPreviewError.isEmpty() && !printedAppVulkanFailure) {
                    std::cerr << "Application Vulkan frame path unavailable: "
                              << materializedPreviewError.toStdString() << "\n";
                    printedAppVulkanFailure = true;
                }
                std::cerr << "FaceStream generator refuses implicit fallback from app Vulkan render to decoder/CPU frame path.\n";
                return 2;
            }
            if (frameImage.isNull()) {
                std::cerr << "Application Vulkan frame path returned a null frame at frame "
                          << frameNumber << ".\n";
                return 2;
            }
            detectionFrameSize = frameImage.size();

            const cv::Mat bgr = jcut::facestream::qImageToBgrMat(frameImage);
            if (options.scrfdDetector) {
                if (!scrfdDetector.isInitialized()) {
                    ScopedStderrSilencer silence(!options.verbose);
                    if (!scrfdDetector.initialize(scrfdParamPath, scrfdBinPath, true, &error)) {
                        std::cerr << "Failed to initialize SCRFD ncnn detector: "
                                  << error.toStdString() << "\n";
                        return 2;
                    }
                }
                QElapsedTimer timer;
                timer.start();
                const QVector<jcut::vulkan_detector::ScrfdDetection> raw =
                    scrfdDetector.inferFromBgr(bgr, tuning.threshold, tuning.scrfdTargetSize, &error);
                vulkanDetectMs = static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
                detections.reserve(raw.size());
                for (const auto& det : raw) {
                    detections.push_back({det.box, det.confidence});
                }
            } else {
                cv::Mat gray;
                cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
                cv::equalizeHist(gray, gray);

                const int minSide = qMax(1, qMin(frameImage.width(), frameImage.height()));
                const double scaleFactor = 1.08;
                const int baseNeighbors = 5;
                const int minDivisor = 22;
                const double minWeight = 0.2;
                const cv::Size minFace(qMax(18, minSide / minDivisor), qMax(18, minSide / minDivisor));
                const cv::Size maxFace(qMax(minFace.width + 1, (minSide * 3) / 4),
                                       qMax(minFace.height + 1, (minSide * 3) / 4));
                std::vector<jcut::facestream::WeightedDetection> weightedDetections;
                auto runCascade = [&](cv::CascadeClassifier& cascade, int minNeighbors, double weightBias) {
                    if (cascade.empty()) {
                        return;
                    }
                    std::vector<cv::Rect> raw;
                    std::vector<int> rejectLevels;
                    std::vector<double> levelWeights;
                    cascade.detectMultiScale(
                        gray, raw, rejectLevels, levelWeights,
                        scaleFactor, minNeighbors, cv::CASCADE_SCALE_IMAGE, minFace, maxFace, true);
                    for (int i = 0; i < static_cast<int>(raw.size()); ++i) {
                        const double weight = (i < static_cast<int>(levelWeights.size()) ? levelWeights[i] : 0.0) + weightBias;
                        if (weight >= minWeight) {
                            weightedDetections.push_back({raw[i], weight});
                        }
                    }
                };
                runCascade(faceCascade, baseNeighbors, 0.0);
                runCascade(faceCascadeAlt, qMax(2, baseNeighbors - 1), -0.05);
                runCascade(faceCascadeProfile, qMax(2, baseNeighbors - 1), -0.10);

                const std::vector<jcut::facestream::WeightedDetection> filtered =
                    jcut::facestream::filterAndSuppressDetections(std::move(weightedDetections), frameImage.size());
                detections.reserve(static_cast<int>(filtered.size()));
                for (const jcut::facestream::WeightedDetection& det : filtered) {
                    detections.push_back({QRectF(det.box.x, det.box.y, det.box.width, det.box.height),
                                          static_cast<float>(det.weight)});
                }
            }
            const QVector<Detection> rawDetections = detections;
            DetectionSanitizeStats sanitizeStats;
            detections = sanitizeDetections(
                detections,
                detectionFrameSize,
                tuning.maxFacesPerFrame,
                tuning,
                &sanitizeStats);
            if (detections.isEmpty()) {
                logDetectionSanitizeStats(
                    frameNumber,
                    detectionFrameSize,
                    tuning,
                    sanitizeStats,
                    rawDetections);
            }
            const QVector<Track> detectionPreviewTracks =
                advanceRuntimeTracks(frameNumber, detections, detectionFrameSize);

            const bool previewFrameDue =
                !previewPipelinePrimed || ((frameNumber % effectivePreviewStride) == 0);
            const bool needsPreviewFrame =
                previewFrameDue &&
                (livePreviewWindow ||
                 previewSocketPtr ||
                 (options.writePreviewFiles && previewWritten < options.previewFrames));
            if (needsPreviewFrame) {
                if (!requireSynchronizedOffscreenPreview(
                        frameNumber,
                        previewFrame,
                        detectionPreviewTracks,
                        detections,
                        QStringLiteral("JCut Materialized Generate FaceStream"),
                        [&]() -> QImage { return frameImage; })) {
                    return 2;
                }
            }
            if (needsPreviewFrame && !previewRequiresSynchronizedDetection) {
                emitPreview(frameImage,
                            frameNumber,
                            detectionPreviewTracks,
                            detections,
                            QStringLiteral("JCut Materialized Generate FaceStream"));
            }
#endif
        }
        if (!recordProcessedFrame(frameOffset,
                                  frameNumber,
                                  renderStats,
                                  detections,
                                  runtimeTracks,
                                  detectionFrameSize,
                                  appVulkanFrame,
                                  decoderVulkanUploadFallback,
                                  hardwareDirectHandoff,
                                  decoderUploadMs,
                                  vulkanDetectMs,
                                  emptyNcnnStats,
                                  decoderFrameHandoff.lastProbe(),
                                  decoderFrameHandoff.lastHardwareDirectAttemptReason())) {
            return 2;
        }
    }
    while (!pendingDecoderDirectSlots.empty()) {
        const int slotIndex = pendingDecoderDirectSlots.front();
        pendingDecoderDirectSlots.pop_front();
        if (!finalizePreparedDecoderSlot(slotIndex)) {
            continue;
        }
    }
    if (options.progress) {
        std::cout << "\n";
    }
    const auto wallEnd = std::chrono::steady_clock::now();
    const QVector<Track> tracks =
        jcut::facestream::buildContinuityTracksFromDetectionFrames(
            rawDetectionFrames,
            trackingTuningForRuntime(tuning));
    const QJsonArray trackRows = jcut::facestream::buildContinuityTrackRows(tracks);
    writeBinaryJsonObject(QDir(options.outputDir).filePath(QStringLiteral("detections.bin")), QJsonObject{
        {QStringLiteral("schema"), QStringLiteral("jcut_facestream_offscreen_detections_v1")},
        {QStringLiteral("video"), options.videoPath},
        {QStringLiteral("backend"), backend},
        {QStringLiteral("frame_domain"), QStringLiteral("source_absolute")},
        {QStringLiteral("frames"), rawDetectionFrames}
    });
    writeBinaryJsonObject(QDir(options.outputDir).filePath(QStringLiteral("tracks.bin")), QJsonObject{
        {QStringLiteral("schema"), QStringLiteral("jcut_facestream_offscreen_tracks_v1")},
        {QStringLiteral("video"), options.videoPath},
        {QStringLiteral("backend"), backend},
        {QStringLiteral("frame_domain"), QStringLiteral("source_absolute")},
        {QStringLiteral("tracks"), trackRows},
        {QStringLiteral("frame_summaries"), frameRows}
    });
    const QJsonObject continuityRoot = jcut::facestream::buildContinuityRoot(
        QStringLiteral("offscreen"),
        false,
        0,
        qMax(0, options.startFrame + totalFrames - 1),
        QJsonArray{},
        trackRows,
        rawDetectionFrames,
        backend);
    writeBinaryJsonObject(QDir(options.outputDir).filePath(QStringLiteral("continuity_facestream.bin")), QJsonObject{
        {QStringLiteral("schema"), QStringLiteral("jcut_facestream_v1")},
        {QStringLiteral("continuity_facestreams_by_clip"), QJsonObject{
            {QStringLiteral("facestream-offscreen-source"), continuityRoot}
        }}
    });

    const bool decodeZeroCopy =
        zeroCopyVulkanDetector &&
        !options.materializedGenerateFacestream &&
        decoderDirectHandoffFrames == processed &&
        hardwareDirectHandoffFrames == processed &&
        appVulkanFramePathFrames == 0 &&
        decoderVulkanUploadFallbackFrames == 0 &&
        processed > 0;
    const QJsonObject summary{
        {QStringLiteral("video"), options.videoPath},
        {QStringLiteral("output_dir"), QDir(options.outputDir).absolutePath()},
        {QStringLiteral("generator_name"), QStringLiteral("JCut DNN FaceStream Generator")},
        {QStringLiteral("backend"), backend},
        {QStringLiteral("detector"), backend},
        {QStringLiteral("max_frames"), totalFrames},
        {QStringLiteral("start_frame"), options.startFrame},
        {QStringLiteral("stride"), tuning.stride},
        {QStringLiteral("runtime_threshold"), tuning.threshold},
        {QStringLiteral("runtime_max_detections"), tuning.maxDetections},
        {QStringLiteral("runtime_max_faces_per_frame"), tuning.maxFacesPerFrame},
        {QStringLiteral("runtime_nms_iou_threshold"), tuning.nmsIouThreshold},
        {QStringLiteral("runtime_track_match_iou_threshold"), tuning.trackMatchIouThreshold},
        {QStringLiteral("runtime_new_track_min_confidence"), tuning.newTrackMinConfidence},
        {QStringLiteral("runtime_primary_face_only"), tuning.primaryFaceOnly},
        {QStringLiteral("runtime_small_face_fallback"), tuning.smallFaceFallback},
        {QStringLiteral("runtime_roi_x1"), tuning.roiX1},
        {QStringLiteral("runtime_roi_y1"), tuning.roiY1},
        {QStringLiteral("runtime_roi_x2"), tuning.roiX2},
        {QStringLiteral("runtime_roi_y2"), tuning.roiY2},
        {QStringLiteral("runtime_min_face_area_ratio"), tuning.minFaceAreaRatio},
        {QStringLiteral("runtime_max_face_area_ratio"), tuning.maxFaceAreaRatio},
        {QStringLiteral("runtime_min_aspect"), tuning.minAspect},
        {QStringLiteral("runtime_max_aspect"), tuning.maxAspect},
        {QStringLiteral("runtime_params_file"), options.paramsFile},
        {QStringLiteral("detector_settings_file"), detectorSettingsPath},
        {QStringLiteral("require_zero_copy"), options.requireZeroCopy},
        {QStringLiteral("require_hardware_vulkan_frame_path"), options.requireHardwareVulkanFramePath},
        {QStringLiteral("allow_cpu_upload_fallback"), options.allowCpuUploadFallback},
        {QStringLiteral("require_zero_copy_satisfied"),
         !options.requireZeroCopy || decodeZeroCopy},
        {QStringLiteral("require_hardware_vulkan_frame_path_satisfied"),
         !options.requireHardwareVulkanFramePath || decoderVulkanUploadFallbackFrames == 0 || hardwareDirectHandoffFrames > 0},
        {QStringLiteral("decoded_frames"), decoded},
        {QStringLiteral("processed_frames"), processed},
        {QStringLiteral("sampling_note"), QStringLiteral("processed_frames increments only on frames where frame_number %% stride == 0")},
        {QStringLiteral("cpu_frames"), cpuFrames},
        {QStringLiteral("hardware_frames"), hardwareFrames},
        {QStringLiteral("app_vulkan_decode_path_frames"), appVulkanFramePathFrames},
        {QStringLiteral("decoder_direct_handoff_frames"), decoderDirectHandoffFrames},
        {QStringLiteral("app_vulkan_frame_path_failed"), appFrameProvider.failed},
        {QStringLiteral("app_vulkan_frame_path_failure"), appFrameProvider.failureReason},
        {QStringLiteral("decoder_vulkan_upload_fallback_frames"), decoderVulkanUploadFallbackFrames},
        {QStringLiteral("hardware_direct_handoff_frames"), hardwareDirectHandoffFrames},
        {QStringLiteral("hardware_interop_probe_supported_frames"), hardwareInteropProbeSupportedFrames},
        {QStringLiteral("hardware_interop_probe_failed_frames"), hardwareInteropProbeFailedFrames},
        {QStringLiteral("last_hardware_interop_probe_path"), lastHardwareInteropProbePath},
        {QStringLiteral("last_hardware_interop_probe_reason"), lastHardwareInteropProbeReason},
        {QStringLiteral("last_hardware_direct_attempt_reason"), lastHardwareDirectAttemptReason},
        {QStringLiteral("total_detections"), totalDetections},
        {QStringLiteral("track_count"), trackRows.size()},
        {QStringLiteral("preview_frames_written"), previewWritten},
        {QStringLiteral("preview_stride"), options.previewStride},
        {QStringLiteral("scrfd_model_variant"), normalizedScrfdModelVariant},
        {QStringLiteral("avg_render_decode_ms"), processed ? renderDecodeMsTotal / processed : 0.0},
        {QStringLiteral("avg_render_composite_ms"), processed ? renderCompositeMsTotal / processed : 0.0},
        {QStringLiteral("avg_render_readback_ms"), processed ? renderReadbackMsTotal / processed : 0.0},
        {QStringLiteral("avg_decoder_vulkan_upload_ms"), processed ? decoderUploadMsTotal / processed : 0.0},
        {QStringLiteral("avg_vulkan_zero_copy_detection_ms"), processed ? vulkanDetectMsTotal / processed : 0.0},
        {QStringLiteral("avg_ncnn_input_ms"), processed ? ncnnInputMsTotal / processed : 0.0},
        {QStringLiteral("avg_ncnn_extract_ms"), processed ? ncnnExtractMsTotal / processed : 0.0},
        {QStringLiteral("avg_ncnn_extract_level8_ms"), processed ? ncnnExtractLevel8MsTotal / processed : 0.0},
        {QStringLiteral("avg_ncnn_extract_level16_ms"), processed ? ncnnExtractLevel16MsTotal / processed : 0.0},
        {QStringLiteral("avg_ncnn_extract_level32_ms"), processed ? ncnnExtractLevel32MsTotal / processed : 0.0},
        {QStringLiteral("avg_ncnn_post_ms"), processed ? ncnnPostMsTotal / processed : 0.0},
        {QStringLiteral("avg_ncnn_total_ms"), processed ? ncnnTotalMsTotal / processed : 0.0},
        {QStringLiteral("wall_sec"), std::chrono::duration<double>(wallEnd - wallStart).count()},
        {QStringLiteral("decode_zero_copy"), decodeZeroCopy},
        {QStringLiteral("qimage_materialized"), options.materializedGenerateFacestream},
        {QStringLiteral("vulkan_path_uses_qimage"), false},
        {QStringLiteral("current_mode_uses_qimage"), options.materializedGenerateFacestream},
        {QStringLiteral("zero_copy_note"),
         options.materializedGenerateFacestream
             ? QStringLiteral("Materialized compatibility mode: frames are rendered by Vulkan, read back to QImage, and scanned by the legacy OpenCV continuity detector.")
             : (options.scrfdDetector
                    ? QStringLiteral("SCRFD ncnn Vulkan zero-copy path: frames are preprocessed from Vulkan image to GPU tensor and consumed by ncnn Vulkan without QImage/cv::Mat detector input materialization.")
                    : (decoderVulkanUploadFallbackFrames > 0
                    ? QStringLiteral("Partial fallback mode: some frames used decoder CPU-image upload into VkImage for detector input after app Vulkan frame path failures; this is not decode zero-copy.")
                    : (processed == 0
                           ? QStringLiteral("No frames were processed by the Vulkan detector path.")
                           : QStringLiteral("Res10 ncnn Vulkan path: frames remain as Vulkan images through preprocessing/inference; only compact detection metadata is read back. No QImage frame materialization is used."))))}
    };
    writeJson(QDir(options.outputDir).filePath(QStringLiteral("summary.json")), summary);
    std::cout << "summary " << jcut::jsonio::serializeCompact(summary).constData() << "\n";
    int resultCode = processed > 0 ? 0 : 1;
    if (options.requireZeroCopy && !decodeZeroCopy) {
        std::cerr << "--require-zero-copy was requested, but the run did not complete with end-to-end zero-copy.\n";
        resultCode = 2;
    }
#ifdef JCUT_FACESTREAM_OFFSCREEN_STANDALONE
    if (livePreviewWindow || detectorControls.window) {
        std::cout.flush();
        std::cerr.flush();
        std::fflush(stdout);
        std::fflush(stderr);
        std::_Exit(resultCode);
    }
#endif
    return resultCode;
}

int runVulkanFacestreamOffscreen(const QStringList& args)
{
    QVector<QByteArray> argvStorage;
    argvStorage.reserve(args.size() + 1);
    argvStorage.push_back(QByteArrayLiteral("jcut_vulkan_facestream_offscreen"));
    for (const QString& arg : args) {
        argvStorage.push_back(arg.toLocal8Bit());
    }

    QVector<char*> argvPointers;
    argvPointers.reserve(argvStorage.size());
    for (QByteArray& token : argvStorage) {
        argvPointers.push_back(token.data());
    }
    return runVulkanFacestreamOffscreenWithArgv(argvPointers.size(), argvPointers.data());
}

#ifdef JCUT_FACESTREAM_OFFSCREEN_STANDALONE
int main(int argc, char** argv)
{
    return runVulkanFacestreamOffscreenWithArgv(argc, argv);
}
#endif
