#include "facestream_generation.h"
#include "facestream_runtime.h"
#include "clip_serialization.h"
#include "detector_settings.h"
#include "decoder_context.h"
#include "decoder_ffmpeg_utils.h"
#include "direct_vulkan_preview_presenter.h"
#include "debug_controls.h"
#include "editor_shared.h"
#include "frame_handle.h"
#include "preview_interaction_state.h"
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
#include <QApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QCheckBox>
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
    int stride = 1;
    int maxDetections = 256;
    int maxFacesPerFrame = 32; // 0 => no post-cap
    int scrfdTargetSize = 640;
    int previewFrames = 24;
    int previewStride = 12;
    float threshold = 0.45f;
    float nmsIouThreshold = 0.35f;
    float trackMatchIouThreshold = 0.22f;
    float newTrackMinConfidence = 0.45f;
    bool primaryFaceOnly = true;
    bool smallFaceFallback = false;
    bool scrfdTiled = false;
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

struct Detection {
    QRectF box;
    float confidence = 0.0f;
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

struct Track {
    int id = -1;
    QRectF box;
    int lastFrame = -1;
    QJsonArray detections;
};

QString backendIdForOptions(const Options& options)
{
    if (options.materializedGenerateFacestream) {
        return QStringLiteral("materialized_generate_facestream_opencv_cascade_v1");
    }
    if (options.heuristicZeroCopyDetector) {
        return QStringLiteral("native_jcut_heuristic_zero_copy_v1");
    }
    if (options.scrfdDetector) {
        return QStringLiteral("scrfd_500m_ncnn_vulkan_zero_copy_v1");
    }
    return QStringLiteral("res10_ssd_ncnn_vulkan_zero_copy_v1");
}

struct RuntimeTuning {
    int stride = 1;
    int maxDetections = 256;
    int scrfdTargetSize = 640;
    int maxFacesPerFrame = 32;
    float threshold = 0.45f;
    float nmsIouThreshold = 0.35f;
    float trackMatchIouThreshold = 0.22f;
    float newTrackMinConfidence = 0.45f;
    bool primaryFaceOnly = true;
    bool smallFaceFallback = false;
    bool scrfdTiled = false;
    float roiX1 = 0.0f;
    float roiY1 = 0.0f;
    float roiX2 = 1.0f;
    float roiY2 = 1.0f;
    float minFaceAreaRatio = 0.0005f;
    float maxFaceAreaRatio = 1.0f;
    float minAspect = 0.45f;
    float maxAspect = 1.80f;
};

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

bool saveRuntimeTuningFile(const QString& path,
                           const RuntimeTuning& tuning,
                           const QString& detector,
                           int scrfdTargetSize,
                           QString* errorMessage = nullptr)
{
    if (path.isEmpty()) {
        return false;
    }
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("failed to write detector settings: %1").arg(path);
        }
        return false;
    }
    const QJsonDocument doc(runtimeTuningToJson(
        tuning,
        detector,
        tuning.scrfdTargetSize > 0 ? tuning.scrfdTargetSize : scrfdTargetSize));
    f.write(doc.toJson(QJsonDocument::Indented));
    f.write("\n");
    return true;
}

double iou(const QRectF& a, const QRectF& b)
{
    const QRectF ix = a.intersected(b);
    const double inter = ix.width() * ix.height();
    const double uni = a.width() * a.height() + b.width() * b.height() - inter;
    return uni > 0.0 ? inter / uni : 0.0;
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
            if (iou(candidate.box, accepted.box) > tuning.nmsIouThreshold) {
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
              << " [--full-video] [--max-faces-per-frame N]"
              << " [--primary-face-only] [--multi-face]"
              << " [--small-face-fallback] [--no-small-face-fallback]"
              << " [--nms-iou F] [--track-match-iou F] [--new-track-min-confidence F]"
              << " [--params-file PATH]"
              << " [--clip-json PATH] [--apply-clip-grading]"
              << " [--detector jcut-dnn|scrfd-ncnn-vulkan|jcut-heuristic-zero-copy]"
              << " [--res10-param PATH] [--res10-bin PATH] [--scrfd-param PATH] [--scrfd-bin PATH]"
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
        } else if (arg == "--max-faces-per-frame") {
            const char* v = next("--max-faces-per-frame");
            if (!v) return false;
            options->maxFacesPerFrame = std::max(0, std::atoi(v));
        } else if (arg == "--scrfd-target-size") {
            const char* v = next("--scrfd-target-size");
            if (!v) return false;
            options->scrfdTargetSize = std::max(320, std::atoi(v));
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
    }
    return true;
}

bool loadClipFromJsonPath(const QString& path, TimelineClip* clipOut, QString* errorOut = nullptr)
{
    if (clipOut) {
        *clipOut = TimelineClip{};
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to open clip json: %1").arg(path);
        }
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to parse clip json %1: %2").arg(path, parseError.errorString());
        }
        return false;
    }
    if (clipOut) {
        *clipOut = editor::clipFromJson(doc.object());
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

void renderProgressLine(int frameOffset,
                        int totalFrames,
                        int frameNumber,
                        int processed,
                        int totalDetections,
                        int currentFrameDetections,
                        double elapsedSec,
                        double avgDetectMs)
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
    const double decodedFps = elapsedSec > 0.0 ? static_cast<double>(current) / elapsedSec : 0.0;
    const double processedFps = elapsedSec > 0.0 ? static_cast<double>(processed) / elapsedSec : 0.0;
    const double etaSec = decodedFps > 0.0 ? static_cast<double>(qMax(0, totalFrames - current)) / decodedFps : -1.0;

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
              << " eta=" << formatDuration(etaSec).toStdString()
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

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        return false;
    }
    const QJsonObject o = doc.object();
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
    *lastAppliedMtime = mtime;
    return true;
}

struct DetectorControlPanel {
    QWidget* window = nullptr;
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

void syncDetectorControlPanel(DetectorControlPanel* panel, const RuntimeTuning& tuning)
{
    if (!panel || !panel->window) {
        return;
    }
    *panel->syncing = true;
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

void updateDetectorRuntimeStats(DetectorControlPanel* panel,
                                int frameOffset,
                                int totalFrames,
                                int frameNumber,
                                int processed,
                                int totalDetections,
                                int currentFrameDetections,
                                double elapsedSec)
{
    if (!panel || !panel->window || !panel->runtimeProgress) {
        return;
    }
    const int current = qBound(0, frameOffset + 1, qMax(1, totalFrames));
    const int progressPercent = static_cast<int>(
        std::round((100.0 * static_cast<double>(current)) / static_cast<double>(qMax(1, totalFrames))));
    const double decodeFps = elapsedSec > 0.0 ? static_cast<double>(current) / elapsedSec : 0.0;
    const double processFps = elapsedSec > 0.0 ? static_cast<double>(processed) / elapsedSec : 0.0;
    const double etaSec = decodeFps > 0.0 ? static_cast<double>(qMax(0, totalFrames - current)) / decodeFps : -1.0;

    panel->runtimeProgress->setValue(qBound(0, progressPercent, 100));
    panel->runtimeFrame->setText(QStringLiteral("%1 / %2 (src frame %3)")
                                     .arg(current)
                                     .arg(totalFrames)
                                     .arg(frameNumber));
    panel->runtimeFps->setText(QStringLiteral("%1").arg(decodeFps, 0, 'f', 2));
    panel->runtimeProcessedFps->setText(QStringLiteral("%1").arg(processFps, 0, 'f', 2));
    panel->runtimeEta->setText(formatDuration(etaSec));
    panel->runtimeElapsed->setText(formatDuration(elapsedSec));
    panel->runtimeDetections->setText(QStringLiteral("%1").arg(totalDetections));
    panel->runtimeTracks->setText(QStringLiteral("%1").arg(currentFrameDetections));
}

DetectorControlPanel createDetectorControlPanel(RuntimeTuning* tuning,
                                                const QString& detector,
                                                int scrfdTargetSize,
                                                const QString& settingsPath,
                                                bool* applyClipGrading,
                                                bool allowApplyClipGrading,
                                                bool* paused)
{
    DetectorControlPanel panel;
    panel.window = new QWidget;
    panel.window->setWindowTitle(QStringLiteral("JCut Detector Settings"));
    panel.window->setMinimumWidth(420);
    auto* root = new QVBoxLayout(panel.window);
    auto* form = new QFormLayout;
    root->addLayout(form);

    auto persist = [tuning, detector, scrfdTargetSize, settingsPath]() {
        QString error;
        if (!saveRuntimeTuningFile(settingsPath,
                                   *tuning,
                                   detector,
                                   tuning && tuning->scrfdTargetSize > 0 ? tuning->scrfdTargetSize : scrfdTargetSize,
                                   &error) &&
            !error.isEmpty()) {
            qWarning().noquote() << error;
        }
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
    statsForm->addRow(QStringLiteral("Frame detections"), panel.runtimeTracks);
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
    return detectRes10VulkanFrame(preprocessor,
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
    return detectScrfdVulkanFrame(preprocessor,
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

void updateTracks(QVector<Track>* tracks,
                  const QVector<Detection>& detections,
                  int frameNumber,
                  const QSize& frameSize,
                  const RuntimeTuning& tuning)
{
    QVector<bool> used(tracks->size(), false);
    for (const Detection& det : detections) {
        int best = -1;
        double bestIou = 0.0;
        for (int i = 0; i < tracks->size(); ++i) {
            if (used.at(i) || frameNumber - tracks->at(i).lastFrame > 48) {
                continue;
            }
            const double score = iou(tracks->at(i).box, det.box);
            if (score > bestIou) {
                bestIou = score;
                best = i;
            }
        }
        const bool forcePrimaryTrack =
            tuning.primaryFaceOnly && tracks->size() == 1 && best >= 0;
        if (best < 0 || (!forcePrimaryTrack && bestIou < tuning.trackMatchIouThreshold)) {
            if (det.confidence < tuning.newTrackMinConfidence) {
                continue;
            }
            if (tuning.primaryFaceOnly && !tracks->isEmpty()) {
                continue;
            }
            Track track;
            track.id = tracks->size();
            track.box = det.box;
            track.lastFrame = frameNumber;
            tracks->push_back(track);
            best = tracks->size() - 1;
            used.push_back(false);
        } else {
            (*tracks)[best].box = QRectF(
                ((*tracks)[best].box.x() * 0.65) + (det.box.x() * 0.35),
                ((*tracks)[best].box.y() * 0.65) + (det.box.y() * 0.35),
                ((*tracks)[best].box.width() * 0.65) + (det.box.width() * 0.35),
                ((*tracks)[best].box.height() * 0.65) + (det.box.height() * 0.35));
            (*tracks)[best].lastFrame = frameNumber;
        }
        used[best] = true;
        const double x = qBound(0.0, det.box.center().x() / qMax(1, frameSize.width()), 1.0);
        const double y = qBound(0.0, det.box.center().y() / qMax(1, frameSize.height()), 1.0);
        const double box = qBound(0.01,
                                  qMax(det.box.width(), det.box.height()) /
                                      static_cast<double>(qMax(1, qMin(frameSize.width(), frameSize.height()))),
                                  1.0);
        (*tracks)[best].detections.append(QJsonObject{
            {QStringLiteral("frame"), frameNumber},
            {QStringLiteral("x"), x},
            {QStringLiteral("y"), y},
            {QStringLiteral("box"), box},
            {QStringLiteral("score"), det.confidence}
        });
    }
}

QImage buildPreview(QImage image,
                    const QVector<Track>& tracks,
                    const QVector<Detection>& detections,
                    const QRectF& roiRect)
{
    Q_UNUSED(tracks)
    QVector<QRect> boxes;
    boxes.reserve(detections.size());
    for (const Detection& detection : detections) {
        boxes.push_back(detection.box.toAlignedRect());
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
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
    return true;
}

bool writeBinaryJsonObject(const QString& path, const QJsonObject& object)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_6_0);
    stream << quint32(0x4A435554); // "JCUT"
    stream << quint32(1);          // schema version
    const QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Compact);
    stream << qCompress(payload, 6);
    return stream.status() == QDataStream::Ok;
}

QJsonObject compactDetectionJson(const Detection& detection, const QSize& frameSize)
{
    return {
        {QStringLiteral("x"), detection.box.x()},
        {QStringLiteral("y"), detection.box.y()},
        {QStringLiteral("w"), detection.box.width()},
        {QStringLiteral("h"), detection.box.height()},
        {QStringLiteral("confidence"), detection.confidence},
        {QStringLiteral("frame_width"), frameSize.width()},
        {QStringLiteral("frame_height"), frameSize.height()}
    };
}

QJsonArray frameTrackDetections(const QVector<Track>& tracks, int frameNumber)
{
    QJsonArray rows;
    for (const Track& track : tracks) {
        if (track.lastFrame != frameNumber || track.detections.isEmpty()) {
            continue;
        }
        QJsonObject row = track.detections.last().toObject();
        row.insert(QStringLiteral("track_id"), track.id);
        row.insert(QStringLiteral("track_box_x"), track.box.x());
        row.insert(QStringLiteral("track_box_y"), track.box.y());
        row.insert(QStringLiteral("track_box_w"), track.box.width());
        row.insert(QStringLiteral("track_box_h"), track.box.height());
        rows.append(row);
    }
    return rows;
}

bool appendBinaryJsonRecord(QFile* file, const QJsonObject& object)
{
    if (!file || !file->isOpen()) {
        return false;
    }
    const QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Compact);
    const QByteArray compressed = qCompress(payload, 6);
    QDataStream stream(file);
    stream.setVersion(QDataStream::Qt_6_0);
    stream << quint32(0x4A465352); // "JFSR"
    stream << quint32(1);          // record schema version
    stream << quint32(compressed.size());
    if (compressed.size() > 0) {
        const qint64 written = file->write(compressed);
        if (written != compressed.size()) {
            return false;
        }
    }
    return stream.status() == QDataStream::Ok && file->flush();
}

struct FaceStreamResumeState {
    QSet<int> completedFrames;
    QJsonArray frameRows;
    QVector<Track> tracks;
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
};

Track* ensureResumeTrack(QVector<Track>* tracks, int trackId)
{
    if (!tracks || trackId < 0) {
        return nullptr;
    }
    for (Track& track : *tracks) {
        if (track.id == trackId) {
            return &track;
        }
    }
    Track track;
    track.id = trackId;
    tracks->push_back(track);
    return &tracks->last();
}

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
        const QByteArray payload = qUncompress(compressed);
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(payload, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            continue;
        }
        const QJsonObject object = doc.object();
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

        const QJsonArray trackDetections = object.value(QStringLiteral("track_detections")).toArray();
        for (const QJsonValue& value : trackDetections) {
            const QJsonObject trackObject = value.toObject();
            Track* track = ensureResumeTrack(&state->tracks, trackObject.value(QStringLiteral("track_id")).toInt(-1));
            if (!track) {
                continue;
            }
            track->lastFrame = frameNumber;
            track->box = QRectF(trackObject.value(QStringLiteral("track_box_x")).toDouble(),
                                trackObject.value(QStringLiteral("track_box_y")).toDouble(),
                                trackObject.value(QStringLiteral("track_box_w")).toDouble(),
                                trackObject.value(QStringLiteral("track_box_h")).toDouble());
            QJsonObject detectionObject = trackObject;
            detectionObject.remove(QStringLiteral("track_id"));
            detectionObject.remove(QStringLiteral("track_box_x"));
            detectionObject.remove(QStringLiteral("track_box_y"));
            detectionObject.remove(QStringLiteral("track_box_w"));
            detectionObject.remove(QStringLiteral("track_box_h"));
            track->detections.append(detectionObject);
        }
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
        options.threshold = detectorSettings.threshold;
        options.nmsIouThreshold = detectorSettings.nmsIouThreshold;
        options.trackMatchIouThreshold = detectorSettings.trackMatchIouThreshold;
        options.newTrackMinConfidence = detectorSettings.newTrackMinConfidence;
        options.primaryFaceOnly = detectorSettings.primaryFaceOnly;
        options.smallFaceFallback = detectorSettings.smallFaceFallback;
        options.scrfdTiled = detectorSettings.scrfdTiled;
    }
    qputenv("JCUT_ALLOW_HEADLESS_HARDWARE_DECODE", "1");
    QDir().mkpath(options.outputDir);
    const bool platformIsOffscreen =
        QString::fromLocal8Bit(qgetenv("QT_QPA_PLATFORM")).compare(QStringLiteral("offscreen"),
                                                                   Qt::CaseInsensitive) == 0;
    QWidget previewHostWindow;
    QWidget* previewWindowPtr = nullptr;
    QLabel* previewStatusLabelPtr = nullptr;
    PreviewInteractionState livePreviewState;
    std::unique_ptr<DirectVulkanPreviewPresenter> livePreviewPresenter;

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
        previewHostWindow.setWindowTitle(QStringLiteral("JCut DNN FaceStream Generator Vulkan Preview"));
        previewHostWindow.resize(1080, 720);
        auto* previewLayout = new QVBoxLayout(&previewHostWindow);
        previewLayout->setContentsMargins(0, 0, 0, 0);
        previewLayout->setSpacing(0);
        auto* previewStatusLabel = new QLabel(QStringLiteral("Waiting for JCut DNN FaceStream preview..."),
                                              &previewHostWindow);
        previewStatusLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        previewStatusLabel->setWordWrap(false);
        previewStatusLabel->setFixedHeight(28);
        previewStatusLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        previewStatusLabel->setStyleSheet(QStringLiteral(
            "background:#161d26; color:#d6e2ef; border:0; padding:6px 10px; "
            "font:600 11px 'DejaVu Sans Mono';"));
        previewLayout->addWidget(previewStatusLabel);

        livePreviewState.viewMode = PreviewSurface::ViewMode::Video;
        livePreviewState.outputSize = renderSize;
        livePreviewState.backgroundColor = QColor(Qt::black);
        livePreviewState.clipCount = 1;
        livePreviewState.selectedClipId = sourceClip.id;
        livePreviewState.clips = QVector<TimelineClip>{sourceClip};
        livePreviewState.tracks = QVector<TimelineTrack>{TimelineTrack{}};
        livePreviewState.playing = true;

        livePreviewPresenter = std::make_unique<DirectVulkanPreviewPresenter>(&livePreviewState, &previewHostWindow);
        if (QWidget* presenterWidget = livePreviewPresenter->widget()) {
            presenterWidget->setMinimumSize(960, 540);
            previewLayout->addWidget(presenterWidget, 1);
        }
        previewHostWindow.show();
        previewWindowPtr = &previewHostWindow;
        previewStatusLabelPtr = previewStatusLabel;
        QElapsedTimer previewInitTimer;
        previewInitTimer.start();
        while (previewInitTimer.elapsed() < 3000 &&
               livePreviewPresenter &&
               !livePreviewPresenter->isActive() &&
               !livePreviewPresenter->hasFailed()) {
            appPtr->processEvents(QEventLoop::AllEvents, 16);
            usleep(16 * 1000);
        }
        if (!livePreviewPresenter || !livePreviewPresenter->isActive()) {
            const QString failureReason =
                livePreviewPresenter && livePreviewPresenter->hasFailed()
                    ? livePreviewPresenter->failureReason().trimmed()
                    : QStringLiteral("Timed out waiting for Vulkan preview presenter initialization.");
            std::cerr << "Failed to initialize Vulkan preview presenter: "
                      << failureReason.toStdString() << "\n";
            return 2;
        }
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
    const bool previewRequested = previewWindowPtr || previewSocketPtr || options.writePreviewFiles;
    const bool previewRequiresSynchronizedDetection =
        (previewWindowPtr != nullptr) || (previewSocketPtr != nullptr);
    const int effectivePreviewStride =
        previewWindowPtr ? 1 : qMax(1, options.previewStride);
    bool previewPipelinePrimed = !previewRequested;
    QString lastPreviewStatusText;
    auto setPreviewStatusText = [&](const QString& text) {
        if (!previewStatusLabelPtr || text == lastPreviewStatusText) {
            return;
        }
        previewStatusLabelPtr->setText(text);
        lastPreviewStatusText = text;
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
    const QString res10ParamPath = findRes10NcnnModelFile(
        options.res10ParamPath, QStringLiteral("res10_300x300_ssd_ncnn.param"));
    const QString res10BinPath = findRes10NcnnModelFile(
        options.res10BinPath, QStringLiteral("res10_300x300_ssd_ncnn.bin"));
    const QString scrfdParamPath = findRes10NcnnModelFile(
        options.scrfdParamPath, QStringLiteral("scrfd_500m-opt2.param"));
    const QString scrfdBinPath = findRes10NcnnModelFile(
        options.scrfdBinPath, QStringLiteral("scrfd_500m-opt2.bin"));
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
        std::cout << "detector=scrfd_500m_ncnn_vulkan_zero_copy_v1"
                  << " name=\"JCut SCRFD FaceStream Generator\""
                  << " inference_backend=ncnn_vulkan"
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
    QVector<Track> tracks;
    QJsonArray frameRows;
    bool printedAppVulkanFailure = false;
    VulkanHarnessContext detectorContext;
    jcut::vulkan_detector::VulkanDetectorFrameHandoff decoderFrameHandoff;
    jcut::vulkan_detector::VulkanZeroCopyFaceDetector zeroCopyDetector;
    jcut::vulkan_detector::VulkanRes10NcnnFaceDetector res10Detector;
    jcut::vulkan_detector::VulkanScrfdNcnnFaceDetector scrfdDetector;
    RuntimeTuning tuning{
        options.stride,
        options.maxDetections,
        options.scrfdTargetSize,
        options.maxFacesPerFrame,
        options.threshold,
        options.nmsIouThreshold,
        options.trackMatchIouThreshold,
        options.newTrackMinConfidence,
        options.primaryFaceOnly,
        options.smallFaceFallback,
        options.scrfdTiled
    };
    QDateTime paramsMtime;
    const QString detectorSettingsPath = detectorSettingsPathForVideo(options.videoPath);
    QDateTime detectorSettingsMtime;
    QFileInfo detectorSettingsInfo(detectorSettingsPath);
    if (applyRuntimeParamsFile(detectorSettingsPath, detectorSettingsInfo, &tuning, &detectorSettingsMtime) &&
        options.verbose) {
        std::cout << "loaded_detector_settings=\"" << detectorSettingsPath.toStdString() << "\"\n";
    }
    DetectorControlPanel detectorControls;
    bool runtimePaused = false;
    if (previewWindowPtr) {
        detectorControls = createDetectorControlPanel(&tuning,
                                                      options.detector,
                                                      options.scrfdTargetSize,
                                                      detectorSettingsPath,
                                                      &options.applyClipGrading,
                                                      !options.clipJsonPath.trimmed().isEmpty(),
                                                      &runtimePaused);
        detectorControls.window->show();
        QString saveError;
        if (saveRuntimeTuningFile(detectorSettingsPath,
                                  tuning,
                                  options.detector,
                                  options.scrfdTargetSize,
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
    auto gradingUsesCurveLut = [](const TimelineClip::GradingKeyframe& grade) -> bool {
        const auto curveDiffersFromIdentity = [](const QVector<QPointF>& points, bool smoothingEnabled) -> bool {
            const QVector<QPointF> identity = defaultGradingCurvePoints();
            const QVector<quint8> lut =
                gradingCurveLut8(points, TimelineClip::kGradingCurveLutSize, smoothingEnabled);
            const QVector<quint8> identityLut =
                gradingCurveLut8(identity, TimelineClip::kGradingCurveLutSize, smoothingEnabled);
            return !lut.isEmpty() && !identityLut.isEmpty() && lut != identityLut;
        };
        return curveDiffersFromIdentity(grade.curvePointsR, grade.curveSmoothingEnabled) ||
               curveDiffersFromIdentity(grade.curvePointsG, grade.curveSmoothingEnabled) ||
               curveDiffersFromIdentity(grade.curvePointsB, grade.curveSmoothingEnabled) ||
               curveDiffersFromIdentity(grade.curvePointsLuma, grade.curveSmoothingEnabled);
    };
    auto applyLivePreviewStatus = [&](VulkanPreviewClipFrameStatus status,
                                      int frameNumber,
                                      const QVector<Track>& tracks,
                                      const QVector<Detection>& detections,
                                      const QString& titlePrefix) -> bool {
        if (!livePreviewPresenter || !previewWindowPtr) {
            return false;
        }
        if (!previewWindowPtr->isVisible() || !livePreviewPresenter->isActive()) {
            return false;
        }
        if (!status.hasFrame) {
            setPreviewStatusText(QStringLiteral("Preview frame unavailable."));
            appPtr->processEvents();
            return false;
        }
        QVector<QRectF> detectionBoxes;
        detectionBoxes.reserve(detections.size());
        QVector<float> detectionConfidences;
        detectionConfidences.reserve(detections.size());
        for (const Detection& det : detections) {
            detectionBoxes.push_back(det.box);
            detectionConfidences.push_back(det.confidence);
        }
        const QVector<VulkanPreviewFacestreamOverlay> overlays =
            jcut::facestream::buildDetectionPreviewOverlays(sourceClip.id,
                                                            frameNumber,
                                                            status.frameSize,
                                                            detectionBoxes,
                                                            detectionConfidences,
                                                            currentRoiRect(status.frameSize));
        const TimelineClip previewClip =
            jcut::facestream::buildFacestreamRenderClip(sourceClip,
                                                        options.videoPath,
                                                        frameNumber,
                                                        frameNumber);
        status.effectsPath = status.sampledFramePregraded
            ? QStringLiteral("offscreen_vulkan_pregraded_passthrough")
            : QStringLiteral("evaluateEffectiveVisualEffectsAtPosition");
        status.gradingBypassed = status.sampledFramePregraded;
        status.correctionsEnabled = false;
        status.correctionsApplied = false;
        status.correctionsSupported = false;
        status.transform = TimelineClip::TransformKeyframe{};
        status.correctionPolygonCount = 0;
        if (status.sampledFramePregraded) {
            status.grading = TimelineClip::GradingKeyframe{};
            status.maskFeather = 0.0;
            status.maskFeatherGamma = 1.0;
            status.curveLutApplied = false;
            status.curveLutSupported = true;
            status.gradingShaderActive = false;
        } else {
            EffectiveVisualEffects effects = evaluateEffectiveVisualEffectsAtPosition(
                previewClip,
                QVector<TimelineTrack>{},
                static_cast<qreal>(frameNumber),
                QVector<RenderSyncMarker>{});
            effects.correctionPolygons.clear();
            status.grading = effects.grading;
            status.maskFeather = effects.maskFeather;
            status.maskFeatherGamma = effects.maskFeatherGamma;
            status.curveLutApplied = gradingUsesCurveLut(effects.grading);
            status.curveLutSupported = true;
            status.gradingShaderActive = true;
        }
        jcut::facestream::updateSingleClipPreviewInteractionState(&livePreviewState,
                                                                  previewClip,
                                                                  frameNumber,
                                                                  status,
                                                                  overlays);
        livePreviewState.playing = false;
        setPreviewStatusText(QStringLiteral("%1 | frame %2 | detections %3")
                                 .arg(titlePrefix)
                                 .arg(frameNumber)
                                 .arg(detections.size()));
        livePreviewPresenter->updateTitle();
        livePreviewPresenter->requestUpdate();
        return true;
    };
    auto updateLivePreviewStateFromDecodedFrame = [&](const editor::FrameHandle& frameHandle,
                                                      int frameNumber,
                                                      const QVector<Track>& tracks,
                                                      const QVector<Detection>& detections,
                                                      const QString& titlePrefix) -> bool {
        return applyLivePreviewStatus(
            jcut::facestream::buildPreviewClipFrameStatus(sourceClip.id,
                                                          frameHandle,
                                                          frameNumber,
                                                          renderSize),
            frameNumber,
            tracks,
            detections,
            titlePrefix);
    };
    auto updateLivePreviewStateFromOffscreenFrame = [&](const render_detail::OffscreenVulkanFrame& frame,
                                                        int frameNumber,
                                                        const QVector<Track>& tracks,
                                                        const QVector<Detection>& detections,
                                                        const QString& titlePrefix) -> bool {
        return applyLivePreviewStatus(
            jcut::facestream::buildPreviewClipFrameStatus(sourceClip.id, frame, frameNumber),
            frameNumber,
            tracks,
            detections,
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
            (previewWindowPtr ||
             previewSocketPtr ||
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
        const bool previewFrameDue =
            !previewPipelinePrimed || ((frameNumber % effectivePreviewStride) == 0);
        if (!previewFrameDue) {
            return true;
        }
        if (!previewRequiresSynchronizedDetection) {
            const QImage previewImage = previewImageProvider();
            if (!previewImage.isNull()) {
                emitPreview(previewImage, frameNumber, tracks, detections, titlePrefix);
            }
            appPtr->processEvents(QEventLoop::AllEvents, 1);
            return true;
        }
        constexpr qint64 kPreviewSyncTimeoutMs = 1500;
        QElapsedTimer syncTimer;
        syncTimer.start();
        bool livePreviewRequested = false;
        while (syncTimer.elapsed() < kPreviewSyncTimeoutMs) {
            if (livePreviewPresenter &&
                !livePreviewRequested &&
                updateLivePreviewStateFromDecodedFrame(frameHandle, frameNumber, tracks, detections, titlePrefix)) {
                livePreviewRequested = true;
            }
            if (livePreviewPresenter && livePreviewRequested) {
                if (livePreviewPresenter->hasFailed()) {
                    setPreviewStatusText(QStringLiteral("Preview failed at frame %1 | %2")
                                             .arg(frameNumber)
                                             .arg(livePreviewPresenter->failureReason()));
                    appPtr->processEvents();
                    return false;
                }
                if (livePreviewPresenter->lastPresentedSourceFrame() == frameNumber &&
                    !livePreviewPresenter->updatePending()) {
                    previewPipelinePrimed = true;
                    setPreviewStatusText(QStringLiteral("%1 | frame %2 | detections %3")
                                             .arg(titlePrefix)
                                             .arg(frameNumber)
                                             .arg(detections.size()));
                    appPtr->processEvents();
                    return true;
                }
            }
            if (!previewWindowPtr) {
                const QImage previewImage = previewImageProvider();
                if (!previewImage.isNull() &&
                    emitPreview(previewImage, frameNumber, tracks, detections, titlePrefix)) {
                    return true;
                }
            }
            setPreviewStatusText(QStringLiteral("Preview waiting for frame %1").arg(frameNumber));
            appPtr->processEvents();
            usleep(16 * 1000);
        }
        setPreviewStatusText(QStringLiteral("Preview sync failed at frame %1").arg(frameNumber));
        appPtr->processEvents();
        std::cerr << "Preview synchronization failed at frame "
                  << frameNumber
                  << ". Detection will not continue without preview.\n";
        return false;
    };
    auto requireSynchronizedOffscreenPreview = [&](int frameNumber,
                                                   const render_detail::OffscreenVulkanFrame& frame,
                                                   const QVector<Track>& tracks,
                                                   const QVector<Detection>& detections,
                                                   const QString& titlePrefix,
                                                   const std::function<QImage()>& previewImageProvider) -> bool {
        const bool previewFrameDue =
            !previewPipelinePrimed || ((frameNumber % effectivePreviewStride) == 0);
        if (!previewFrameDue) {
            return true;
        }
        if (!previewRequiresSynchronizedDetection) {
            const QImage previewImage = previewImageProvider();
            if (!previewImage.isNull()) {
                emitPreview(previewImage, frameNumber, tracks, detections, titlePrefix);
            }
            appPtr->processEvents(QEventLoop::AllEvents, 1);
            return true;
        }
        constexpr qint64 kPreviewSyncTimeoutMs = 1500;
        QElapsedTimer syncTimer;
        syncTimer.start();
        bool livePreviewRequested = false;
        while (syncTimer.elapsed() < kPreviewSyncTimeoutMs) {
            if (livePreviewPresenter &&
                !livePreviewRequested &&
                updateLivePreviewStateFromOffscreenFrame(frame, frameNumber, tracks, detections, titlePrefix)) {
                livePreviewRequested = true;
            }
            if (livePreviewPresenter && livePreviewRequested) {
                if (livePreviewPresenter->hasFailed()) {
                    setPreviewStatusText(QStringLiteral("Preview failed at frame %1 | %2")
                                             .arg(frameNumber)
                                             .arg(livePreviewPresenter->failureReason()));
                    appPtr->processEvents();
                    return false;
                }
                if (livePreviewPresenter->lastPresentedSourceFrame() == frameNumber &&
                    !livePreviewPresenter->updatePending()) {
                    previewPipelinePrimed = true;
                    setPreviewStatusText(QStringLiteral("%1 | frame %2 | detections %3")
                                             .arg(titlePrefix)
                                             .arg(frameNumber)
                                             .arg(detections.size()));
                    appPtr->processEvents();
                    return true;
                }
            }
            if (!previewWindowPtr) {
                const QImage previewImage = previewImageProvider();
                if (!previewImage.isNull() &&
                    emitPreview(previewImage, frameNumber, tracks, detections, titlePrefix)) {
                    return true;
                }
            }
            setPreviewStatusText(QStringLiteral("Preview waiting for frame %1").arg(frameNumber));
            appPtr->processEvents();
            usleep(16 * 1000);
        }
        setPreviewStatusText(QStringLiteral("Preview sync failed at frame %1").arg(frameNumber));
        appPtr->processEvents();
        std::cerr << "Preview synchronization failed at frame "
                  << frameNumber
                  << ". Detection will not continue without preview.\n";
        return false;
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
            if (livePreviewPresenter) {
                render_detail::OffscreenVulkanFrame warmupFrame;
                jcut::facestream::VulkanFrameStats warmupStats;
                QString warmupError;
                if (jcut::facestream::renderFrameToVulkan(&appFrameProvider,
                                                          sourceClip,
                                                          options.videoPath,
                                                          previewStartFrame,
                                                          previewStartFrame,
                                                          renderSize,
                                                          &warmupFrame,
                                                          &warmupStats,
                                                          &warmupError) &&
                    updateLivePreviewStateFromOffscreenFrame(
                        warmupFrame,
                        previewStartFrame,
                        warmupTracks,
                        warmupDetections,
                        QStringLiteral("JCut DNN FaceStream Generator (Preview Warmup)"))) {
                    QElapsedTimer presentTimer;
                    presentTimer.start();
                    while (presentTimer.elapsed() < 500) {
                        if (livePreviewPresenter->hasFailed()) {
                            return false;
                        }
                        if (livePreviewPresenter->lastPresentedSourceFrame() == previewStartFrame &&
                            !livePreviewPresenter->updatePending()) {
                            previewPipelinePrimed = true;
                            appPtr->processEvents();
                            return true;
                        }
                        appPtr->processEvents(QEventLoop::AllEvents, 16);
                        usleep(16 * 1000);
                    }
                }
                return false;
            }
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
                return true;
            }
            return false;
        };

        if (previewStatusLabelPtr) {
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
            if (previewStatusLabelPtr) {
                setPreviewStatusText(
                    QStringLiteral("Waiting for preview frame %1 | %2 ms")
                        .arg(previewStartFrame)
                        .arg(previewWarmupTimer.elapsed()));
            }
            if (livePreviewPresenter && livePreviewPresenter->hasFailed()) {
                const QString failureReason = livePreviewPresenter->failureReason().trimmed().isEmpty()
                    ? QStringLiteral("The preview presenter failed during warmup.")
                    : livePreviewPresenter->failureReason().trimmed();
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
        if (previewPipelinePrimed && previewStatusLabelPtr) {
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
    tracks = resume.tracks;
    frameRows = resume.frameRows;

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
    for (int frameOffset = resumeStartOffset; frameOffset < totalFrames; ++frameOffset) {
        const int frameNumber = options.startFrame + frameOffset;
        if (resume.completedFrames.contains(frameNumber)) {
            ++decoded;
            const bool previewFrameDue =
                !previewPipelinePrimed || ((frameNumber % effectivePreviewStride) == 0);
            if (previewWindowPtr && previewFrameDue) {
                render_detail::OffscreenVulkanFrame resumedFrame;
                jcut::facestream::VulkanFrameStats resumedStats;
                QString resumedError;
                const QVector<Track> resumedTracks;
                const QVector<Detection> resumedDetections;
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
                        []() -> QImage { return QImage(); })) {
                    std::cerr << "Preview synchronization failed while advancing resumed frame "
                              << frameNumber
                              << ". "
                              << resumedError.toStdString()
                              << "\n";
                    return 2;
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
                                           elapsedSec);
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
                                   processed > 0 ? vulkanDetectMsTotal / processed : 0.0);
            }
            continue;
        }
        if (previewWindowPtr || detectorControls.window) {
            appPtr->processEvents();
            if (detectorControls.window && !detectorControls.window->isVisible()) {
                detectorControls.window = nullptr;
            }
        }
        while (runtimePaused) {
            if (previewStatusLabelPtr) {
                setPreviewStatusText(QStringLiteral(
                    "FaceStream detection paused. Resume from the runtime controls to continue."));
            }
            appPtr->processEvents(QEventLoop::AllEvents, 16);
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
            if (applyRuntimeParamsFile(options.paramsFile, paramsInfo, &tuning, &paramsMtime)) {
                syncDetectorControlPanel(&detectorControls, tuning);
                if (options.verbose) {
                    std::cout << "runtime_params"
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
        if (applyRuntimeParamsFile(detectorSettingsPath, liveSettingsInfo, &tuning, &detectorSettingsMtime)) {
            syncDetectorControlPanel(&detectorControls, tuning);
            if (options.verbose) {
                std::cout << "detector_settings"
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
                                           elapsedSec);
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
                if (!requireSynchronizedPreview(
                        frameNumber,
                        decodedFrame,
                        tracks,
                        detections,
                        QStringLiteral("JCut DNN FaceStream Generator (Decoder Vulkan Upload Fallback)"),
                        [&]() -> QImage {
                            if (decodedFrame.hasCpuImage()) {
                                return decodedFrame.cpuImage();
                            }
                            editor::FrameHandle previewFrame = decoder.decodeFrame(frameNumber);
                            return previewFrame.hasCpuImage() ? previewFrame.cpuImage() : QImage();
                        })) {
                    return false;
                }
                if (!previewRequiresSynchronizedDetection && decodedFrame.hasCpuImage()) {
                    emitPreview(decodedFrame.cpuImage(),
                                frameNumber,
                                tracks,
                                detections,
                                QStringLiteral("JCut DNN FaceStream Generator (Decoder Vulkan Upload Fallback)"));
                }
                return true;
            };

            render_detail::OffscreenVulkanFrame vulkanFrame;
            error.clear();
            // Always route detection through the app render path so the grading shader
            // is active for every frame. When clip grading is not requested, the clip
            // carries neutral grading and the shader behaves as a passthrough.
            const bool preferDecoderDirect = false;
            bool frameProcessed = false;
            if (preferDecoderDirect) {
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
                const bool previewFrameDue =
                    !previewPipelinePrimed || ((frameNumber % effectivePreviewStride) == 0);
                const bool needsPreviewFrame =
                    previewFrameDue &&
                    (previewWindowPtr ||
                     previewSocketPtr ||
                     (options.writePreviewFiles && previewWritten < options.previewFrames));
                if (needsPreviewFrame) {
                    if (!requireSynchronizedOffscreenPreview(
                            frameNumber,
                            vulkanFrame,
                            tracks,
                            detections,
                            QStringLiteral("JCut DNN FaceStream Generator"),
                            [&]() -> QImage {
                                if (previewWindowPtr) {
                                    return QImage();
                                }
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
            QImage frameImage = jcut::facestream::renderFrameWithVulkan(&appFrameProvider,
                                                                       sourceClip,
                                                                       options.videoPath,
                                                                       frameNumber,
                                                                       frameNumber,
                                                                       renderSize,
                                                                       &renderStats);
            appVulkanFrame = !frameImage.isNull();
            if (appVulkanFrame) {
                ++hardwareFrames;
            } else {
                if (appFrameProvider.failed && !printedAppVulkanFailure) {
                    std::cerr << "Application Vulkan frame path unavailable: "
                              << appFrameProvider.failureReason.toStdString() << "\n";
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

            const bool previewFrameDue =
                !previewPipelinePrimed || ((frameNumber % effectivePreviewStride) == 0);
            const bool needsPreviewFrame =
                previewFrameDue &&
                (previewWindowPtr ||
                 previewSocketPtr ||
                 (options.writePreviewFiles && previewWritten < options.previewFrames));
            if (needsPreviewFrame) {
                if (!requireSynchronizedPreview(
                        frameNumber,
                        editor::FrameHandle{},
                        tracks,
                        detections,
                        QStringLiteral("JCut Materialized Generate FaceStream"),
                        [&]() -> QImage { return frameImage; })) {
                    return 2;
                }
            }
            if (needsPreviewFrame && !previewRequiresSynchronizedDetection) {
                emitPreview(frameImage,
                            frameNumber,
                            tracks,
                            detections,
                            QStringLiteral("JCut Materialized Generate FaceStream"));
            }
#endif
        }
        ++processed;
        totalDetections += detections.size();
        renderDecodeMsTotal += renderStats.decodeMs;
        renderCompositeMsTotal += renderStats.compositeMs;
        renderReadbackMsTotal += renderStats.readbackMs;
        vulkanDetectMsTotal += vulkanDetectMs;
        const QString detectorId = backend;
        const bool qimageMaterialized = options.materializedGenerateFacestream;
        QJsonArray detectionRows;
        for (const Detection& detection : detections) {
            detectionRows.append(compactDetectionJson(detection, detectionFrameSize));
        }
        const bool decoderDirectHandoff = !appVulkanFrame && hardwareDirectHandoff;
        const QJsonObject frameRow{
            {QStringLiteral("frame"), frameNumber},
            {QStringLiteral("detector"), detectorId},
            {QStringLiteral("detections"), detections.size()},
            {QStringLiteral("tracks"), 0},
            {QStringLiteral("app_vulkan_frame_path"), appVulkanFrame},
            {QStringLiteral("app_render_decode_ms"), renderStats.decodeMs},
            {QStringLiteral("app_render_texture_ms"), renderStats.textureMs},
            {QStringLiteral("app_render_composite_ms"), renderStats.compositeMs},
            {QStringLiteral("app_render_readback_ms"), renderStats.readbackMs},
            {QStringLiteral("vulkan_zero_copy_detection_ms"), vulkanDetectMs},
            {QStringLiteral("decoder_vulkan_upload_ms"), decoderUploadMs},
            {QStringLiteral("decoder_vulkan_upload_fallback"), decoderVulkanUploadFallback},
            {QStringLiteral("hardware_direct_handoff"), hardwareDirectHandoff},
            {QStringLiteral("hardware_direct_attempt_reason"), decoderFrameHandoff.lastHardwareDirectAttemptReason()},
            {QStringLiteral("qimage_materialized"), qimageMaterialized}
        };
        frameRows.append(frameRow);
        QJsonObject streamRow = frameRow;
        streamRow.insert(QStringLiteral("type"), QStringLiteral("frame"));
        streamRow.insert(QStringLiteral("schema"), QStringLiteral("jcut_facestream_frame_v1"));
        streamRow.insert(QStringLiteral("video"), options.videoPath);
        streamRow.insert(QStringLiteral("backend"), backend);
        streamRow.insert(QStringLiteral("decoder_direct_handoff"), decoderDirectHandoff);
        streamRow.insert(QStringLiteral("hardware_interop_probe_supported"), decoderFrameHandoff.lastProbe().supported);
        streamRow.insert(QStringLiteral("hardware_interop_probe_failed"), !decoderFrameHandoff.lastProbe().path.isEmpty() && !decoderFrameHandoff.lastProbe().supported);
        streamRow.insert(QStringLiteral("hardware_interop_probe_path"), decoderFrameHandoff.lastProbe().path);
        streamRow.insert(QStringLiteral("hardware_interop_probe_reason"), decoderFrameHandoff.lastProbe().reason);
        streamRow.insert(QStringLiteral("hardware_frame"), appVulkanFrame || hardwareDirectHandoff);
        streamRow.insert(QStringLiteral("cpu_frame"), qimageMaterialized || decoderVulkanUploadFallback);
        streamRow.insert(QStringLiteral("detection_boxes"), detectionRows);
        streamRow.insert(QStringLiteral("track_detections"), QJsonArray{});
        if (!appendBinaryJsonRecord(&faceStreamFile, streamRow)) {
            std::cerr << "Failed to append streaming facestream checkpoint: "
                      << faceStreamPath.toStdString() << "\n";
            return 2;
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
                      << " decoder_vulkan_upload_fallback=" << (decoderVulkanUploadFallback ? 1 : 0)
                      << " hardware_direct_handoff=" << (hardwareDirectHandoff ? 1 : 0)
                      << " hardware_direct_attempt_reason=\"" << decoderFrameHandoff.lastHardwareDirectAttemptReason().toStdString() << "\""
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
                               processed > 0 ? vulkanDetectMsTotal / processed : 0.0);
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
                                       detections.size(),
                                       elapsedSec);
        }
    }
    if (options.progress) {
        std::cout << "\n";
    }
    const auto wallEnd = std::chrono::steady_clock::now();
    const QJsonArray trackRows;
    writeBinaryJsonObject(QDir(options.outputDir).filePath(QStringLiteral("tracks.bin")), QJsonObject{
        {QStringLiteral("schema"), QStringLiteral("jcut_facestream_offscreen_tracks_v1")},
        {QStringLiteral("video"), options.videoPath},
        {QStringLiteral("backend"), backend},
        {QStringLiteral("tracks"), trackRows},
        {QStringLiteral("frames"), frameRows}
    });
    const QJsonObject continuityRoot = jcut::facestream::buildContinuityRoot(
        QStringLiteral("offscreen"),
        false,
        0,
        qMax(0, options.startFrame + totalFrames - 1),
        QJsonArray{},
        QJsonArray{},
        frameRows,
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
        {QStringLiteral("track_count"), 0},
        {QStringLiteral("preview_frames_written"), previewWritten},
        {QStringLiteral("preview_stride"), options.previewStride},
        {QStringLiteral("avg_render_decode_ms"), processed ? renderDecodeMsTotal / processed : 0.0},
        {QStringLiteral("avg_render_composite_ms"), processed ? renderCompositeMsTotal / processed : 0.0},
        {QStringLiteral("avg_render_readback_ms"), processed ? renderReadbackMsTotal / processed : 0.0},
        {QStringLiteral("avg_decoder_vulkan_upload_ms"), processed ? decoderUploadMsTotal / processed : 0.0},
        {QStringLiteral("avg_vulkan_zero_copy_detection_ms"), processed ? vulkanDetectMsTotal / processed : 0.0},
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
    std::cout << "summary " << QJsonDocument(summary).toJson(QJsonDocument::Compact).constData() << "\n";
    if (options.requireZeroCopy && !decodeZeroCopy) {
        std::cerr << "--require-zero-copy was requested, but the run did not complete with end-to-end zero-copy.\n";
        return 2;
    }
    return processed > 0 ? 0 : 1;
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
