#include <opencv2/core.hpp>
#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>

#include "decoder_context.h"
#include "debug_controls.h"
#include "editor_shared.h"
#include "frame_handle.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <QString>
#include <QImage>
#include <numeric>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace {

struct Options {
    std::string videoPath = "nasreen.mp4";
    std::string backend = "vkcom";
    int maxFrames = 120;
    int stride = 12;
    float threshold = 0.30f;
    std::string proxyPath;
    std::string decodeMode = "software";
    bool requireZeroCopy = false;
};

struct Detection {
    float confidence = 0.0f;
    cv::Rect box;
};

std::string avError(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

void usage(const char* argv0)
{
    std::cerr << "Usage: " << argv0 << " [video] [--backend vkcom|cuda|cpu] [--max-frames N] [--stride N] [--threshold F] [--proxy PATH|none] [--decode software] [--require-zero-copy]\n";
}

bool parseArgs(int argc, char** argv, Options* options)
{
    if (!options) {
        return false;
    }
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto requireValue = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << name << "\n";
                return nullptr;
            }
            return argv[++i];
        };
        if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            return false;
        } else if (arg == "--backend") {
            const char* v = requireValue("--backend");
            if (!v) return false;
            options->backend = v;
        } else if (arg == "--max-frames") {
            const char* v = requireValue("--max-frames");
            if (!v) return false;
            options->maxFrames = std::max(1, std::atoi(v));
        } else if (arg == "--stride") {
            const char* v = requireValue("--stride");
            if (!v) return false;
            options->stride = std::max(1, std::atoi(v));
        } else if (arg == "--threshold") {
            const char* v = requireValue("--threshold");
            if (!v) return false;
            options->threshold = std::max(0.0f, std::min(1.0f, static_cast<float>(std::atof(v))));
        } else if (arg == "--proxy") {
            const char* v = requireValue("--proxy");
            if (!v) return false;
            options->proxyPath = v;
        } else if (arg == "--decode") {
            const char* v = requireValue("--decode");
            if (!v) return false;
            options->decodeMode = v;
        } else if (arg == "--require-zero-copy") {
            options->requireZeroCopy = true;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "Unknown argument: " << arg << "\n";
            usage(argv[0]);
            return false;
        } else {
            options->videoPath = arg;
        }
    }
    return true;
}

bool hasTarget(cv::dnn::Backend backend, cv::dnn::Target target)
{
    const std::vector<cv::dnn::Target> targets = cv::dnn::getAvailableTargets(backend);
    return std::find(targets.begin(), targets.end(), target) != targets.end();
}

void printTargets(const char* label, cv::dnn::Backend backend)
{
    const std::vector<cv::dnn::Target> targets = cv::dnn::getAvailableTargets(backend);
    std::cout << label << " targets=" << targets.size();
    for (cv::dnn::Target target : targets) {
        std::cout << ' ' << static_cast<int>(target);
    }
    std::cout << '\n';
}

bool configureBackend(const Options& options, cv::dnn::Net* net)
{
    if (!net) {
        return false;
    }
    if (options.backend == "vkcom" || options.backend == "vulkan") {
        if (!hasTarget(cv::dnn::DNN_BACKEND_VKCOM, cv::dnn::DNN_TARGET_VULKAN)) {
            std::cerr << "VKCOM/Vulkan DNN target is unavailable in this OpenCV build/runtime.\n";
            return false;
        }
        net->setPreferableBackend(cv::dnn::DNN_BACKEND_VKCOM);
        net->setPreferableTarget(cv::dnn::DNN_TARGET_VULKAN);
        return true;
    }
    if (options.backend == "cuda") {
        if (hasTarget(cv::dnn::DNN_BACKEND_CUDA, cv::dnn::DNN_TARGET_CUDA_FP16)) {
            net->setPreferableBackend(cv::dnn::DNN_BACKEND_CUDA);
            net->setPreferableTarget(cv::dnn::DNN_TARGET_CUDA_FP16);
            return true;
        }
        if (hasTarget(cv::dnn::DNN_BACKEND_CUDA, cv::dnn::DNN_TARGET_CUDA)) {
            net->setPreferableBackend(cv::dnn::DNN_BACKEND_CUDA);
            net->setPreferableTarget(cv::dnn::DNN_TARGET_CUDA);
            return true;
        }
        std::cerr << "CUDA DNN target is unavailable in this OpenCV build/runtime.\n";
        return false;
    }
    if (options.backend == "cpu") {
        net->setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
        net->setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
        return true;
    }
    std::cerr << "Unsupported backend: " << options.backend << "\n";
    return false;
}

std::vector<Detection> runFaceDnn(cv::dnn::Net* net, const cv::Mat& bgr, float threshold)
{
    std::vector<Detection> detections;
    if (!net || bgr.empty()) {
        return detections;
    }
    cv::Mat blob = cv::dnn::blobFromImage(
        bgr, 1.0, cv::Size(300, 300), cv::Scalar(104.0, 177.0, 123.0), false, false);
    net->setInput(blob);
    cv::Mat output = net->forward();
    if (output.dims != 4 || output.size[2] <= 0 || output.size[3] < 7) {
        return detections;
    }
    const int count = output.size[2];
    for (int i = 0; i < count; ++i) {
        const float* row = output.ptr<float>(0, 0, i);
        const float confidence = row[2];
        if (confidence < threshold) {
            continue;
        }
        int x1 = static_cast<int>(row[3] * bgr.cols);
        int y1 = static_cast<int>(row[4] * bgr.rows);
        int x2 = static_cast<int>(row[5] * bgr.cols);
        int y2 = static_cast<int>(row[6] * bgr.rows);
        x1 = std::clamp(x1, 0, std::max(0, bgr.cols - 1));
        y1 = std::clamp(y1, 0, std::max(0, bgr.rows - 1));
        x2 = std::clamp(x2, 0, std::max(0, bgr.cols - 1));
        y2 = std::clamp(y2, 0, std::max(0, bgr.rows - 1));
        const int w = std::max(0, x2 - x1);
        const int h = std::max(0, y2 - y1);
        if (w >= 8 && h >= 8) {
            detections.push_back(Detection{confidence, cv::Rect(x1, y1, w, h)});
        }
    }
    return detections;
}


bool applyDecodeMode(const std::string& mode)
{
    if (mode != "software") {
        std::cerr << "This OpenCV inference binary currently requires CPU images from the established DecoderContext. "
                  << "Use --decode software. True zero-copy must use VulkanZeroCopyFaceDetector plus a Vulkan-native inference backend, not OpenCV blobFromImage.\n";
        return false;
    }
    editor::setDebugDecodePreference(editor::DecodePreference::Software);
    return true;
}

TimelineClip makeClipForVideo(const Options& options)
{
    TimelineClip clip;
    clip.filePath = QString::fromStdString(options.videoPath);
    clip.mediaType = ClipMediaType::Video;
    clip.sourceKind = MediaSourceKind::File;
    clip.videoEnabled = true;
    clip.useProxy = options.proxyPath != "none";
    if (!options.proxyPath.empty() && options.proxyPath != "none") {
        clip.proxyPath = QString::fromStdString(options.proxyPath);
    }
    return clip;
}

cv::Mat qImageToBgrMat(const QImage& image)
{
    if (image.isNull()) {
        return {};
    }
    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
    cv::Mat rgbaMat(rgba.height(), rgba.width(), CV_8UC4,
                    const_cast<uchar*>(rgba.constBits()),
                    static_cast<size_t>(rgba.bytesPerLine()));
    cv::Mat bgr;
    cv::cvtColor(rgbaMat, bgr, cv::COLOR_RGBA2BGR);
    return bgr;
}



} // namespace

int main(int argc, char** argv)
{
    Options options;
    if (!parseArgs(argc, argv, &options)) {
        return 2;
    }
    if (!std::filesystem::exists(options.videoPath)) {
        std::cerr << "Video not found: " << options.videoPath << "\n";
        return 2;
    }

    const std::string proto = "external/opencv/samples/dnn/face_detector/deploy.prototxt";
    const std::string model = "external/opencv/samples/dnn/face_detector/res10_300x300_ssd_iter_140000_fp16.caffemodel";
    if (!std::filesystem::exists(proto) || !std::filesystem::exists(model)) {
        std::cerr << "OpenCV face detector model assets are missing.\n";
        return 2;
    }

    printTargets("cuda", cv::dnn::DNN_BACKEND_CUDA);
    printTargets("vkcom", cv::dnn::DNN_BACKEND_VKCOM);

    cv::dnn::Net net = cv::dnn::readNetFromCaffe(proto, model);
    if (!configureBackend(options, &net)) {
        return 2;
    }

    if (!applyDecodeMode(options.decodeMode)) {
        return 2;
    }

    const TimelineClip clip = makeClipForVideo(options);
    const QString playbackPath = playbackMediaPathForClip(clip);
    if (playbackPath.isEmpty()) {
        std::cerr << "No playback media path resolved for clip.\n";
        return 2;
    }

    editor::DecoderContext decoder(playbackPath);
    if (!decoder.initialize()) {
        std::cerr << "DecoderContext failed to initialize: " << playbackPath.toStdString() << "\n";
        return 2;
    }

    std::cout << "established_decode=true"
              << " requested_video=" << options.videoPath
              << " playback_path=" << playbackPath.toStdString()
              << " decode_mode=" << options.decodeMode
              << " decoder_path=" << decoder.info().decodePath.toStdString()
              << " interop_path=" << decoder.info().interopPath.toStdString()
              << " hardware=" << (decoder.info().hardwareAccelerated ? 1 : 0)
              << " proxy_active=" << (playbackPath != QString::fromStdString(options.videoPath) ? 1 : 0)
              << '\n';

    std::vector<double> inferMs;
    int decoded = 0;
    int inferred = 0;
    int framesWithFaces = 0;
    int totalDetections = 0;
    int cpuFrames = 0;
    int hardwareFrames = 0;
    int gpuTextureFrames = 0;
    int skippedNonCpuFrames = 0;

    const auto wallStart = std::chrono::steady_clock::now();
    for (int frameNumber = 0; frameNumber < options.maxFrames; ++frameNumber) {
        editor::FrameHandle frame = decoder.decodeFrame(frameNumber);
        if (frame.isNull()) {
            break;
        }
        ++decoded;
        if (frame.hasCpuImage()) ++cpuFrames;
        if (frame.hasHardwareFrame()) ++hardwareFrames;
        if (frame.hasGpuTexture()) ++gpuTextureFrames;
        if ((frameNumber % options.stride) != 0) {
            continue;
        }
        if (!frame.hasCpuImage()) {
            ++skippedNonCpuFrames;
            std::cout << "frame=" << frameNumber
                      << " skipped_non_cpu=1"
                      << " has_hardware=" << (frame.hasHardwareFrame() ? 1 : 0)
                      << " has_gpu_texture=" << (frame.hasGpuTexture() ? 1 : 0)
                      << '\n';
            continue;
        }
        const cv::Mat bgr = qImageToBgrMat(frame.cpuImage());
        const auto start = std::chrono::steady_clock::now();
        std::vector<Detection> detections = runFaceDnn(&net, bgr, options.threshold);
        const auto end = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(end - start).count();
        inferMs.push_back(ms);
        ++inferred;
        totalDetections += static_cast<int>(detections.size());
        if (!detections.empty()) {
            ++framesWithFaces;
        }
        std::cout << "frame=" << frameNumber
                  << " infer_ms=" << std::fixed << std::setprecision(2) << ms
                  << " detections=" << detections.size();
        for (const Detection& det : detections) {
            std::cout << " conf=" << std::setprecision(3) << det.confidence
                      << " box=" << det.box.x << ',' << det.box.y << ',' << det.box.width << 'x' << det.box.height;
        }
        std::cout << '\n';
    }
    const auto wallEnd = std::chrono::steady_clock::now();

    const double avg = inferMs.empty()
        ? 0.0
        : std::accumulate(inferMs.begin(), inferMs.end(), 0.0) / static_cast<double>(inferMs.size());
    const double maxMs = inferMs.empty() ? 0.0 : *std::max_element(inferMs.begin(), inferMs.end());
    const double wallSec = std::chrono::duration<double>(wallEnd - wallStart).count();

    const bool zeroCopy = (gpuTextureFrames > 0 || hardwareFrames > 0) && skippedNonCpuFrames == 0;

    std::cout << "summary backend=" << options.backend
              << " video=" << options.videoPath
              << " decoded=" << decoded
              << " inferred=" << inferred
              << " frames_with_faces=" << framesWithFaces
              << " detections=" << totalDetections
              << " avg_infer_ms=" << std::fixed << std::setprecision(2) << avg
              << " max_infer_ms=" << maxMs
              << " wall_sec=" << wallSec
              << " cpu_frames=" << cpuFrames
              << " hardware_frames=" << hardwareFrames
              << " gpu_texture_frames=" << gpuTextureFrames
              << " skipped_non_cpu_frames=" << skippedNonCpuFrames
              << " zero_copy=" << (zeroCopy ? 1 : 0)
              << '\n';
    if (options.requireZeroCopy && !zeroCopy) {
        std::cerr << "required zero-copy path was not used; failing benchmark\n";
        return 3;
    }
    return inferred > 0 ? 0 : 1;
}
