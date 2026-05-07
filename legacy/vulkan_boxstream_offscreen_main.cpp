#include "boxstream_generation.h"
#include "boxstream_runtime.h"
#include "decoder_context.h"
#include "debug_controls.h"
#include "editor_shared.h"
#include "frame_handle.h"
#include "vulkan_res10_ncnn_face_detector.h"
#include "vulkan_detector_frame_handoff.h"
#include "vulkan_scrfd_ncnn_face_detector.h"
#include "vulkan_zero_copy_face_detector.h"

#include <QDir>
#include <QDataStream>
#include <QElapsedTimer>
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
#include <QPixmap>
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
    QString outputDir = QStringLiteral("testbench_assets/vulkan_boxstream_offscreen");
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
    QString detector = QStringLiteral("jcut-dnn");
    QString res10ParamPath;
    QString res10BinPath;
    QString scrfdParamPath;
    QString scrfdBinPath;
    QString paramsFile;
    QString previewSocket;
    bool livePreview = false;
    bool writePreviewFiles = false;
    bool materializedGenerateBoxstream = false;
    bool heuristicZeroCopyDetector = false;
    bool scrfdDetector = false;
    bool requireHardwareVulkanFramePath = false;
    bool allowCpuUploadFallback = false;
    bool verbose = false;
    int logInterval = 0; // 0 => summary only unless --verbose is set
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

struct Track {
    int id = -1;
    QRectF box;
    int lastFrame = -1;
    QJsonArray detections;
};

struct RuntimeTuning {
    int stride = 1;
    int maxDetections = 256;
    int maxFacesPerFrame = 32;
    float threshold = 0.45f;
    float nmsIouThreshold = 0.35f;
    float trackMatchIouThreshold = 0.22f;
    float newTrackMinConfidence = 0.45f;
    bool primaryFaceOnly = true;
    bool smallFaceFallback = false;
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
        {QStringLiteral("scrfd_target_size"), scrfdTargetSize},
        {QStringLiteral("stride"), tuning.stride},
        {QStringLiteral("max_detections"), tuning.maxDetections},
        {QStringLiteral("max_faces_per_frame"), tuning.maxFacesPerFrame},
        {QStringLiteral("threshold"), tuning.threshold},
        {QStringLiteral("nms_iou_threshold"), tuning.nmsIouThreshold},
        {QStringLiteral("track_match_iou_threshold"), tuning.trackMatchIouThreshold},
        {QStringLiteral("new_track_min_confidence"), tuning.newTrackMinConfidence},
        {QStringLiteral("primary_face_only"), tuning.primaryFaceOnly},
        {QStringLiteral("small_face_fallback"), tuning.smallFaceFallback},
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
    const QJsonDocument doc(runtimeTuningToJson(tuning, detector, scrfdTargetSize));
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
                                      const RuntimeTuning& tuning)
{
    QVector<Detection> out;
    if (!frameSize.isValid()) {
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
            continue;
        }
        QRectF box = d.box.intersected(QRectF(0, 0, frameSize.width(), frameSize.height()));
        if (box.width() <= 1.0 || box.height() <= 1.0) {
            continue;
        }
        const QPointF c = box.center();
        if (!roiRect.contains(c)) {
            continue;
        }
        const double areaRatio = (box.width() * box.height()) / qMax(1.0, frameArea);
        if (areaRatio < tuning.minFaceAreaRatio || areaRatio > tuning.maxFaceAreaRatio) {
            continue;
        }
        const double aspect = box.width() / qMax(1.0, box.height());
        if (aspect < tuning.minAspect || aspect > tuning.maxAspect) {
            continue;
        }
        out.push_back({box, d.confidence});
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
                keep = false;
                break;
            }
        }
        if (keep) {
            suppressed.push_back(candidate);
        }
    }
    out = suppressed;
    if (tuning.primaryFaceOnly && out.size() > 1) {
        out.resize(1);
    }
    if (maxFacesPerFrame > 0 && out.size() > maxFacesPerFrame) {
        out.resize(maxFacesPerFrame);
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
              << " [--detector jcut-dnn|scrfd-ncnn-vulkan|jcut-heuristic-zero-copy]"
              << " [--res10-param PATH] [--res10-bin PATH] [--scrfd-param PATH] [--scrfd-bin PATH]"
              << " [--scrfd-target-size N]"
              << " [--preview-window] [--no-preview-window] [--preview-files]"
              << " [--materialized-generate-boxstream]"
              << " [--require-hardware-vulkan-frame-path]"
              << " [--allow-cpu-upload-fallback]"
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
            } else if (detector != QStringLiteral("jcut-dnn") &&
                detector != QStringLiteral("jcut-dnn-vulkan") &&
                detector != QStringLiteral("res10-vulkan") &&
                detector != QStringLiteral("native-jcut-dnn") &&
                detector != QStringLiteral("native_vulkan_dnn")) {
                std::cerr << "Unsupported --detector value: " << v
                          << ". Use jcut-dnn, scrfd-ncnn-vulkan, or jcut-heuristic-zero-copy.\n";
                return false;
            } else {
                options->heuristicZeroCopyDetector = false;
                options->scrfdDetector = false;
                options->detector = QStringLiteral("jcut-dnn");
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
        } else if (arg == "--preview-socket") {
            const char* v = next("--preview-socket");
            if (!v) return false;
            options->previewSocket = QString::fromLocal8Bit(v);
        } else if (arg == "--preview-window") {
            options->livePreview = true;
        } else if (arg == "--no-preview-window") {
            options->livePreview = false;
        } else if (arg == "--preview-files") {
            options->writePreviewFiles = true;
        } else if (arg == "--no-preview-files") {
            options->writePreviewFiles = false;
        } else if (arg == "--materialized-generate-boxstream") {
            options->materializedGenerateBoxstream = true;
        } else if (arg == "--require-hardware-vulkan-frame-path") {
            options->requireHardwareVulkanFramePath = true;
        } else if (arg == "--allow-cpu-upload-fallback") {
            options->allowCpuUploadFallback = true;
        } else if (arg == "--verbose") {
            options->verbose = true;
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
    return true;
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
    QSlider* threshold = nullptr;
    QLabel* thresholdValue = nullptr;
    QSlider* nms = nullptr;
    QLabel* nmsValue = nullptr;
    QSlider* trackMatch = nullptr;
    QLabel* trackMatchValue = nullptr;
    QSlider* newTrack = nullptr;
    QLabel* newTrackValue = nullptr;
    QSlider* minArea = nullptr;
    QLabel* minAreaValue = nullptr;
    QSlider* maxFaces = nullptr;
    QLabel* maxFacesValue = nullptr;
    QCheckBox* primaryFaceOnly = nullptr;
    QCheckBox* smallFaceFallback = nullptr;
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

void syncDetectorControlPanel(DetectorControlPanel* panel, const RuntimeTuning& tuning)
{
    if (!panel || !panel->window) {
        return;
    }
    *panel->syncing = true;
    panel->threshold->setValue(qRound(tuning.threshold * 100.0f));
    panel->thresholdValue->setText(percentText(tuning.threshold));
    panel->nms->setValue(qRound(tuning.nmsIouThreshold * 100.0f));
    panel->nmsValue->setText(percentText(tuning.nmsIouThreshold));
    panel->trackMatch->setValue(qRound(tuning.trackMatchIouThreshold * 100.0f));
    panel->trackMatchValue->setText(percentText(tuning.trackMatchIouThreshold));
    panel->newTrack->setValue(qRound(tuning.newTrackMinConfidence * 100.0f));
    panel->newTrackValue->setText(percentText(tuning.newTrackMinConfidence));
    panel->minArea->setValue(qRound(tuning.minFaceAreaRatio * 100000.0f));
    panel->minAreaValue->setText(areaRatioText(tuning.minFaceAreaRatio));
    panel->maxFaces->setValue(tuning.maxFacesPerFrame);
    panel->maxFacesValue->setText(tuning.maxFacesPerFrame == 0
                                      ? QStringLiteral("unlimited")
                                      : QString::number(tuning.maxFacesPerFrame));
    panel->primaryFaceOnly->setChecked(tuning.primaryFaceOnly);
    panel->smallFaceFallback->setChecked(tuning.smallFaceFallback);
    *panel->syncing = false;
}

DetectorControlPanel createDetectorControlPanel(RuntimeTuning* tuning,
                                                const QString& detector,
                                                int scrfdTargetSize,
                                                const QString& settingsPath)
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
        if (!saveRuntimeTuningFile(settingsPath, *tuning, detector, scrfdTargetSize, &error) &&
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
    addSlider(QStringLiteral("Track match IoU"),
              QStringLiteral("How much a new detection must overlap an existing track to continue it. Lower values tolerate camera/subject motion; higher values prevent unrelated faces from stealing a track."),
              0, 100, qRound(tuning->trackMatchIouThreshold * 100.0f),
              &panel.trackMatch, &panel.trackMatchValue,
              [](int v) { return percentText(v / 100.0f); },
              [tuning](int v) { tuning->trackMatchIouThreshold = v / 100.0f; });
    addSlider(QStringLiteral("New track confidence"),
              QStringLiteral("Minimum confidence required to create a brand-new track. Lower this when valid faces are not starting tracks; raise it if noisy detections create extra tracks."),
              1, 99, qRound(tuning->newTrackMinConfidence * 100.0f),
              &panel.newTrack, &panel.newTrackValue,
              [](int v) { return percentText(v / 100.0f); },
              [tuning](int v) { tuning->newTrackMinConfidence = v / 100.0f; });
    addSlider(QStringLiteral("Min face area"),
              QStringLiteral("Smallest accepted face box as a percent of frame area. Lower this for distant/small faces; raise it to reject tiny false positives."),
              0, 2000, qRound(tuning->minFaceAreaRatio * 100000.0f),
              &panel.minArea, &panel.minAreaValue,
              [](int v) { return areaRatioText(v / 100000.0f); },
              [tuning](int v) { tuning->minFaceAreaRatio = v / 100000.0f; });
    addSlider(QStringLiteral("Max faces/frame"),
              QStringLiteral("Maximum accepted faces per processed frame after filtering. 0 means unlimited. Use 1 or Primary mode for single-speaker videos; increase for panels or crowds."),
              0, 64, tuning->maxFacesPerFrame,
              &panel.maxFaces, &panel.maxFacesValue,
              [](int v) { return v == 0 ? QStringLiteral("unlimited") : QString::number(v); },
              [tuning](int v) { tuning->maxFacesPerFrame = v; });

    panel.primaryFaceOnly = new QCheckBox(QStringLiteral("Keep only strongest face"));
    panel.primaryFaceOnly->setToolTip(QStringLiteral("When enabled, only the strongest face detection is kept and only one track is allowed. Use this for single-subject clips to avoid track explosions."));
    panel.primaryFaceOnly->setChecked(tuning->primaryFaceOnly);
    form->addRow(QStringLiteral("Primary mode"), panel.primaryFaceOnly);
    {
        const std::shared_ptr<bool> syncing = panel.syncing;
        QObject::connect(panel.primaryFaceOnly, &QCheckBox::toggled, panel.window, [syncing, tuning, persist](bool checked) {
            if (*syncing || !tuning) {
                return;
            }
            tuning->primaryFaceOnly = checked;
            persist();
        });
    }

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
        if (!ownsInstanceAndDevice &&
            physicalDevice == context.physicalDevice &&
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
    if (!jcut::boxstream::ensureFaceDnnModel(QDir::currentPath(), &proto, &model)) {
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
                    const QVector<Detection>& detections)
{
    QVector<QRect> boxes;
    boxes.reserve(detections.size());
    for (const Detection& detection : detections) {
        boxes.push_back(detection.box.toAlignedRect());
    }
    return jcut::boxstream::buildScanPreview(image, boxes, tracks.size());
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

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    Options options;
    if (!parseArgs(argc, argv, &options)) {
        return 2;
    }
    if (!QFileInfo::exists(options.videoPath)) {
        std::cerr << "Video not found: " << options.videoPath.toStdString() << "\n";
        return 2;
    }
    qputenv("JCUT_ALLOW_HEADLESS_HARDWARE_DECODE", "1");
    QDir().mkpath(options.outputDir);
    const bool platformIsOffscreen =
        QString::fromLocal8Bit(qgetenv("QT_QPA_PLATFORM")).compare(QStringLiteral("offscreen"),
                                                                   Qt::CaseInsensitive) == 0;
    QLabel previewWindow;
    QLabel* previewWindowPtr = nullptr;
    if (options.livePreview && !platformIsOffscreen) {
        previewWindow.setWindowTitle(QStringLiteral("JCut DNN FaceStream Generator Offscreen Preview"));
        previewWindow.setAlignment(Qt::AlignCenter);
        previewWindow.setMinimumSize(960, 540);
        previewWindow.setStyleSheet(QStringLiteral("background:#111; color:#ddd; border:1px solid #333;"));
        previewWindow.setText(QStringLiteral("Waiting for JCut DNN FaceStream preview..."));
        previewWindow.show();
        previewWindowPtr = &previewWindow;
        app.processEvents();
    }

    editor::setDebugDecodePreference(options.decodePreference);
    editor::DecoderContext decoder(options.videoPath);
    if (!decoder.initialize()) {
        std::cerr << "Failed to initialize decoder: " << options.videoPath.toStdString() << "\n";
        return 2;
    }
    TimelineClip sourceClip;
    sourceClip.id = QStringLiteral("boxstream-offscreen-source");
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
    jcut::boxstream::VulkanFrameProvider appFrameProvider;
    const QSize renderSize = decoder.info().frameSize.isValid()
        ? decoder.info().frameSize
        : QSize(1920, 1080);
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

    QString error;
#if JCUT_HAVE_OPENCV
    cv::CascadeClassifier faceCascade;
    cv::CascadeClassifier faceCascadeAlt;
    cv::CascadeClassifier faceCascadeProfile;
    if (options.materializedGenerateBoxstream) {
        const QString cascadePath = jcut::boxstream::findCascadeFile(QStringLiteral("haarcascade_frontalface_default.xml"));
        if (cascadePath.isEmpty() || !faceCascade.load(cascadePath.toStdString())) {
            std::cerr << "Failed to load Haar cascade for materialized Generate FaceStream scan.\n";
            return 2;
        }
        const QString altCascadePath = jcut::boxstream::findCascadeFile(QStringLiteral("haarcascade_frontalface_alt2.xml"));
        if (!altCascadePath.isEmpty()) {
            faceCascadeAlt.load(altCascadePath.toStdString());
        }
        const QString profileCascadePath = jcut::boxstream::findCascadeFile(QStringLiteral("haarcascade_profileface.xml"));
        if (!profileCascadePath.isEmpty()) {
            faceCascadeProfile.load(profileCascadePath.toStdString());
        }
    }
#else
    if (options.materializedGenerateBoxstream) {
        std::cerr << "OpenCV is not enabled in this build; materialized Generate FaceStream scan is unavailable.\n";
        return 2;
    }
    if (options.scrfdDetector) {
        std::cerr << "OpenCV is not enabled in this build; SCRFD materialized-input detector is unavailable.\n";
        return 2;
    }
#endif
    const bool zeroCopyVulkanDetector = !options.materializedGenerateBoxstream && !options.scrfdDetector;
    const QString res10ParamPath = findRes10NcnnModelFile(
        options.res10ParamPath, QStringLiteral("res10_300x300_ssd_ncnn.param"));
    const QString res10BinPath = findRes10NcnnModelFile(
        options.res10BinPath, QStringLiteral("res10_300x300_ssd_ncnn.bin"));
    const QString scrfdParamPath = findRes10NcnnModelFile(
        options.scrfdParamPath, QStringLiteral("scrfd_500m-opt2.param"));
    const QString scrfdBinPath = findRes10NcnnModelFile(
        options.scrfdBinPath, QStringLiteral("scrfd_500m-opt2.bin"));
    if (zeroCopyVulkanDetector && !options.heuristicZeroCopyDetector) {
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
        std::cout << "detector=scrfd_500m_ncnn_vulkan_materialized_input_v1"
                  << " name=\"JCut SCRFD FaceStream Generator\""
                  << " inference_backend=ncnn_vulkan"
                  << " model_param=\"" << scrfdParamPath.toStdString() << "\""
                  << " model_bin=\"" << scrfdBinPath.toStdString() << "\""
                  << " qimage_materialized=1\n";
    }

    int decoded = 0;
    int processed = 0;
    int cpuFrames = 0;
    int hardwareFrames = 0;
    int totalDetections = 0;
    int previewWritten = 0;
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
        options.maxFacesPerFrame,
        options.threshold,
        options.nmsIouThreshold,
        options.trackMatchIouThreshold,
        options.newTrackMinConfidence,
        options.primaryFaceOnly,
        options.smallFaceFallback
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
    if (previewWindowPtr) {
        detectorControls = createDetectorControlPanel(&tuning,
                                                      options.detector,
                                                      options.scrfdTargetSize,
                                                      detectorSettingsPath);
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
        app.processEvents();
    }

    const auto wallStart = std::chrono::steady_clock::now();
    const int totalFrames = static_cast<int>(qMax<int64_t>(1, targetFrames));
    for (int frameOffset = 0; frameOffset < totalFrames; ++frameOffset) {
        const int frameNumber = options.startFrame + frameOffset;
        if (previewWindowPtr || detectorControls.window) {
            app.processEvents();
            if (detectorControls.window && !detectorControls.window->isVisible()) {
                detectorControls.window = nullptr;
            }
        }
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
            continue;
        }

        jcut::boxstream::VulkanFrameStats renderStats;
        QVector<Detection> detections;
        QSize detectionFrameSize = renderSize;
        bool appVulkanFrame = false;
        bool decoderVulkanUploadFallback = false;
        bool hardwareDirectHandoff = false;
        double vulkanDetectMs = 0.0;
        double decoderUploadMs = 0.0;

        if (zeroCopyVulkanDetector) {
            auto emitPreview = [&](const QImage& image, const QString& titlePrefix) {
                if (image.isNull()) {
                    return;
                }
                const bool previewFrameDue = (frameNumber % options.previewStride) == 0;
                const bool needsPreviewFrame =
                    previewFrameDue &&
                    (previewWindowPtr ||
                     previewSocketPtr ||
                     (options.writePreviewFiles && previewWritten < options.previewFrames));
                if (!needsPreviewFrame) {
                    return;
                }
                const QImage preview = buildPreview(image, tracks, detections);
                if (previewSocketPtr) {
                    sendPreviewFrame(previewSocketPtr, preview);
                }
                if (previewWindowPtr && !preview.isNull()) {
                    previewWindowPtr->setPixmap(QPixmap::fromImage(preview).scaled(
                        previewWindowPtr->size(),
                        Qt::KeepAspectRatio,
                        Qt::SmoothTransformation));
                    previewWindowPtr->setWindowTitle(QStringLiteral("%1 - frame %2 tracks %3")
                                                         .arg(titlePrefix)
                                                         .arg(frameNumber)
                                                         .arg(tracks.size()));
                    app.processEvents();
                    if (!previewWindowPtr->isVisible()) {
                        options.livePreview = false;
                        previewWindowPtr = nullptr;
                    }
                }
                if (options.writePreviewFiles && previewWritten < options.previewFrames) {
                    const QString previewPath = QDir(options.outputDir).filePath(
                        QStringLiteral("preview_%1.png").arg(frameNumber, 6, 10, QChar('0')));
                    preview.save(previewPath);
                    ++previewWritten;
                }
            };

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
                detections = sanitizeDetections(detections, detectionFrameSize, tuning.maxFacesPerFrame, tuning);
                if (decodedFrame.hasCpuImage()) {
                    emitPreview(decodedFrame.cpuImage(),
                                QStringLiteral("JCut DNN FaceStream Generator (Decoder Vulkan Upload Fallback)"));
                }
                return true;
            };

            render_detail::OffscreenVulkanFrame vulkanFrame;
            error.clear();
            if (!jcut::boxstream::renderFrameToVulkan(&appFrameProvider,
                                                      sourceClip,
                                                      options.videoPath,
                                                      frameNumber,
                                                      frameNumber,
                                                      renderSize,
                                                      &vulkanFrame,
                                                      &renderStats,
                                                      &error)) {
                if (!printedAppVulkanFailure) {
                    std::cerr << "Application Vulkan frame path unavailable: "
                              << error.toStdString() << "\n";
                    printedAppVulkanFailure = true;
                }
                if (!processDecoderFrameDirectly()) {
                    continue;
                }
            } else {
                appVulkanFrame = true;
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
                    continue;
                }
                detections = sanitizeDetections(detections, detectionFrameSize, tuning.maxFacesPerFrame, tuning);
                const bool previewFrameDue = (frameNumber % options.previewStride) == 0;
                const bool needsPreviewFrame =
                    previewFrameDue &&
                    (previewWindowPtr ||
                     previewSocketPtr ||
                     (options.writePreviewFiles && previewWritten < options.previewFrames));
                if (needsPreviewFrame) {
                    QImage frameImage = jcut::boxstream::readLastRenderedVulkanFrameImage(&appFrameProvider,
                                                                                           &renderStats,
                                                                                           &error);
                    if (!frameImage.isNull()) {
                        emitPreview(frameImage, QStringLiteral("JCut DNN FaceStream Generator"));
                    } else if (!error.isEmpty() && !printedAppVulkanFailure) {
                        std::cerr << "Preview readback unavailable: " << error.toStdString() << "\n";
                        printedAppVulkanFailure = true;
                    }
                }
            }
        } else {
#if JCUT_HAVE_OPENCV
            QImage frameImage = jcut::boxstream::renderFrameWithVulkan(&appFrameProvider,
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
                editor::FrameHandle frame = decoder.decodeFrame(frameNumber);
                if (frame.isNull()) {
                    break;
                }
                if (frame.hasHardwareFrame() || frame.hasGpuTexture()) ++hardwareFrames;
                if (!frame.hasCpuImage()) {
                    continue;
                }
                frameImage = frame.cpuImage();
                ++cpuFrames;
            }
            if (frameImage.isNull()) {
                continue;
            }
            detectionFrameSize = frameImage.size();

            const cv::Mat bgr = jcut::boxstream::qImageToBgrMat(frameImage);
            if (options.scrfdDetector) {
                if (!scrfdDetector.isInitialized()) {
                    ScopedStderrSilencer silence(!options.verbose);
                    if (!scrfdDetector.initialize(scrfdParamPath, scrfdBinPath, true, &error)) {
                        std::cerr << "Failed to initialize SCRFD ncnn detector: "
                                  << error.toStdString() << "\n";
                        continue;
                    }
                }
                QElapsedTimer timer;
                timer.start();
                const QVector<jcut::vulkan_detector::ScrfdDetection> raw =
                    scrfdDetector.inferFromBgr(bgr, tuning.threshold, options.scrfdTargetSize, &error);
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
                std::vector<jcut::boxstream::WeightedDetection> weightedDetections;
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

                const std::vector<jcut::boxstream::WeightedDetection> filtered =
                    jcut::boxstream::filterAndSuppressDetections(std::move(weightedDetections), frameImage.size());
                detections.reserve(static_cast<int>(filtered.size()));
                for (const jcut::boxstream::WeightedDetection& det : filtered) {
                    detections.push_back({QRectF(det.box.x, det.box.y, det.box.width, det.box.height),
                                          static_cast<float>(det.weight)});
                }
            }
            detections = sanitizeDetections(detections, detectionFrameSize, tuning.maxFacesPerFrame, tuning);

            const bool previewFrameDue = (frameNumber % options.previewStride) == 0;
            const bool needsPreviewFrame =
                previewFrameDue &&
                (previewWindowPtr ||
                 previewSocketPtr ||
                 (options.writePreviewFiles && previewWritten < options.previewFrames));
            if (needsPreviewFrame) {
                const QImage preview = buildPreview(frameImage, tracks, detections);
                if (previewSocketPtr) {
                    sendPreviewFrame(previewSocketPtr, preview);
                }
                if (previewWindowPtr && !preview.isNull()) {
                    previewWindowPtr->setPixmap(QPixmap::fromImage(preview).scaled(
                        previewWindowPtr->size(),
                        Qt::KeepAspectRatio,
                        Qt::SmoothTransformation));
                    previewWindowPtr->setWindowTitle(QStringLiteral(
                        "JCut Materialized Generate FaceStream - frame %1 tracks %2")
                        .arg(frameNumber)
                        .arg(tracks.size()));
                    app.processEvents();
                    if (!previewWindowPtr->isVisible()) {
                        options.livePreview = false;
                        previewWindowPtr = nullptr;
                    }
                }
                if (options.writePreviewFiles && previewWritten < options.previewFrames) {
                    const QString previewPath = QDir(options.outputDir).filePath(
                        QStringLiteral("preview_%1.png").arg(frameNumber, 6, 10, QChar('0')));
                    preview.save(previewPath);
                    ++previewWritten;
                }
            }
#endif
        }
        updateTracks(&tracks, detections, frameNumber, detectionFrameSize, tuning);

        ++processed;
        totalDetections += detections.size();
        renderDecodeMsTotal += renderStats.decodeMs;
        renderCompositeMsTotal += renderStats.compositeMs;
        renderReadbackMsTotal += renderStats.readbackMs;
        vulkanDetectMsTotal += vulkanDetectMs;
        const QString detectorId = options.materializedGenerateBoxstream
            ? QStringLiteral("materialized_generate_facestream_opencv_cascade_v1")
            : (options.scrfdDetector
                   ? QStringLiteral("scrfd_500m_ncnn_vulkan_materialized_input_v1")
                   : (options.heuristicZeroCopyDetector
                          ? QStringLiteral("native_jcut_heuristic_zero_copy_v1")
                          : QStringLiteral("res10_ssd_ncnn_vulkan_zero_copy_v1")));
        const bool qimageMaterialized = options.materializedGenerateBoxstream || options.scrfdDetector;
        frameRows.append(QJsonObject{
            {QStringLiteral("frame"), frameNumber},
            {QStringLiteral("detector"), detectorId},
            {QStringLiteral("detections"), detections.size()},
            {QStringLiteral("tracks"), tracks.size()},
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
        });
        const bool logThisFrame =
            options.verbose ||
            (options.logInterval > 0 && (processed % options.logInterval) == 0);
        if (logThisFrame) {
            std::cout << "frame=" << frameNumber
                      << " detector=" << detectorId.toStdString()
                      << " detections=" << detections.size()
                      << " tracks=" << tracks.size()
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
    }
    const auto wallEnd = std::chrono::steady_clock::now();
    QJsonArray trackRows;
    for (const Track& track : tracks) {
        trackRows.append(QJsonObject{
            {QStringLiteral("track_id"), track.id},
            {QStringLiteral("last_frame"), track.lastFrame},
            {QStringLiteral("length"), track.detections.size()},
            {QStringLiteral("detections"), track.detections}
        });
    }
    const QString backend = options.materializedGenerateBoxstream
        ? QStringLiteral("materialized_generate_facestream_opencv_cascade_v1")
        : (options.scrfdDetector
               ? QStringLiteral("scrfd_500m_ncnn_vulkan_materialized_input_v1")
               : (options.heuristicZeroCopyDetector
                      ? QStringLiteral("native_jcut_heuristic_zero_copy_v1")
                      : QStringLiteral("res10_ssd_ncnn_vulkan_zero_copy_v1")));
    writeJson(QDir(options.outputDir).filePath(QStringLiteral("tracks.json")), QJsonObject{
        {QStringLiteral("schema"), QStringLiteral("jcut_boxstream_offscreen_tracks_v1")},
        {QStringLiteral("video"), options.videoPath},
        {QStringLiteral("backend"), backend},
        {QStringLiteral("tracks"), trackRows},
        {QStringLiteral("frames"), frameRows}
    });
    const QJsonArray streams = jcut::boxstream::buildContinuityStreams(
        trackRows,
        QJsonObject{},
        backend,
        false);
    const QJsonObject continuityRoot = jcut::boxstream::buildContinuityRoot(
        QStringLiteral("offscreen"),
        false,
        0,
        qMax(0, options.startFrame + totalFrames - 1),
        streams);
    writeJson(QDir(options.outputDir).filePath(QStringLiteral("continuity_boxstream.json")), QJsonObject{
        {QStringLiteral("schema"), QStringLiteral("jcut_boxstream_v1")},
        {QStringLiteral("continuity_boxstreams_by_clip"), QJsonObject{
            {QStringLiteral("boxstream-offscreen-source"), continuityRoot}
        }}
    });

    const bool decodeZeroCopy =
        zeroCopyVulkanDetector &&
        !options.materializedGenerateBoxstream &&
        !options.scrfdDetector &&
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
        {QStringLiteral("require_hardware_vulkan_frame_path"), options.requireHardwareVulkanFramePath},
        {QStringLiteral("allow_cpu_upload_fallback"), options.allowCpuUploadFallback},
        {QStringLiteral("require_hardware_vulkan_frame_path_satisfied"),
         !options.requireHardwareVulkanFramePath || decoderVulkanUploadFallbackFrames == 0 || hardwareDirectHandoffFrames > 0},
        {QStringLiteral("decoded_frames"), decoded},
        {QStringLiteral("processed_frames"), processed},
        {QStringLiteral("sampling_note"), QStringLiteral("processed_frames increments only on frames where frame_number %% stride == 0")},
        {QStringLiteral("cpu_frames"), cpuFrames},
        {QStringLiteral("hardware_frames"), hardwareFrames},
        {QStringLiteral("app_vulkan_decode_path_frames"), hardwareFrames},
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
        {QStringLiteral("track_count"), tracks.size()},
        {QStringLiteral("preview_frames_written"), previewWritten},
        {QStringLiteral("preview_stride"), options.previewStride},
        {QStringLiteral("avg_render_decode_ms"), processed ? renderDecodeMsTotal / processed : 0.0},
        {QStringLiteral("avg_render_composite_ms"), processed ? renderCompositeMsTotal / processed : 0.0},
        {QStringLiteral("avg_render_readback_ms"), processed ? renderReadbackMsTotal / processed : 0.0},
        {QStringLiteral("avg_decoder_vulkan_upload_ms"), processed ? decoderUploadMsTotal / processed : 0.0},
        {QStringLiteral("avg_vulkan_zero_copy_detection_ms"), processed ? vulkanDetectMsTotal / processed : 0.0},
        {QStringLiteral("wall_sec"), std::chrono::duration<double>(wallEnd - wallStart).count()},
        {QStringLiteral("decode_zero_copy"), decodeZeroCopy},
        {QStringLiteral("qimage_materialized"), options.materializedGenerateBoxstream || options.scrfdDetector},
        {QStringLiteral("vulkan_path_uses_qimage"), false},
        {QStringLiteral("current_mode_uses_qimage"), options.materializedGenerateBoxstream || options.scrfdDetector},
        {QStringLiteral("zero_copy_note"),
         options.materializedGenerateBoxstream
             ? QStringLiteral("Materialized compatibility mode: frames are rendered by Vulkan, read back to QImage, and scanned by the legacy OpenCV continuity detector.")
             : (options.scrfdDetector
                    ? QStringLiteral("SCRFD ncnn Vulkan inference mode: frame input is currently materialized to QImage/cv::Mat before ncnn inference; this improves small-face recall but is not detector zero-copy.")
                    : (decoderVulkanUploadFallbackFrames > 0
                    ? QStringLiteral("Partial fallback mode: some frames used decoder CPU-image upload into VkImage for detector input after app Vulkan frame path failures; this is not decode zero-copy.")
                    : (processed == 0
                           ? QStringLiteral("No frames were processed by the Vulkan detector path.")
                           : QStringLiteral("Res10 ncnn Vulkan path: frames remain as Vulkan images through preprocessing/inference; only compact detection metadata is read back. No QImage frame materialization is used."))))}
    };
    writeJson(QDir(options.outputDir).filePath(QStringLiteral("summary.json")), summary);
    std::cout << "summary " << QJsonDocument(summary).toJson(QJsonDocument::Compact).constData() << "\n";
    return processed > 0 ? 0 : 1;
}
