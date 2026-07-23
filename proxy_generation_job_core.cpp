#include "proxy_generation_job_core.h"

#include "standalone_export_renderer.h"
#include "standalone_timeline_renderer.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>

#include <nlohmann/json.hpp>

namespace jcut {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

const char* stateName(ProxyGenerationJobSnapshot::State state)
{
    using State = ProxyGenerationJobSnapshot::State;
    switch (state) {
    case State::Idle: return "idle";
    case State::Starting: return "starting";
    case State::Running: return "running";
    case State::Canceling: return "canceling";
    case State::Completed: return "completed";
    case State::Canceled: return "canceled";
    case State::Failed: return "failed";
    }
    return "failed";
}

const char* formatName(ProxyGenerationFormat format)
{
    switch (format) {
    case ProxyGenerationFormat::ImageSequenceJpeg:
        return "image_sequence_jpeg";
    case ProxyGenerationFormat::H264Mp4:
        return "h264_mp4";
    case ProxyGenerationFormat::MjpegMov:
        return "mjpeg_mov";
    }
    return "unknown";
}

std::optional<std::int64_t> imageSequenceResumeFrame(
    const fs::path& directory)
{
    std::error_code error;
    if (!fs::is_directory(directory, error) || error) return std::nullopt;
    std::int64_t maximum = -1;
    for (const fs::directory_entry& entry :
         fs::directory_iterator(directory, error)) {
        if (error) return std::nullopt;
        if (!entry.is_regular_file(error) || error) continue;
        const std::string extension = entry.path().extension().string();
        if (extension != ".jpeg" && extension != ".jpg" &&
            extension != ".png") {
            continue;
        }
        const std::string stem = entry.path().stem().string();
        if (!stem.starts_with("frame_")) continue;
        const std::string digits = stem.substr(6);
        if (digits.empty() ||
            !std::all_of(digits.begin(), digits.end(), [](char value) {
                return value >= '0' && value <= '9';
            })) {
            continue;
        }
        try {
            maximum = std::max(
                maximum,
                static_cast<std::int64_t>(std::stoll(digits)));
        } catch (...) {
        }
    }
    return maximum >= 0
        ? std::optional<std::int64_t>(maximum + 1)
        : std::nullopt;
}

bool writeManifest(const ProxyGenerationJobSnapshot& snapshot,
                   const ProxyGenerationJobRequest& request)
{
    if (snapshot.manifestPath.empty()) return false;
    const fs::path path(snapshot.manifestPath);
    const fs::path temporary = path.string() + ".tmp";
    const json root{
        {"schema", "jcut_processing_job_v1"},
        {"operation", "proxy_generation"},
        {"status", stateName(snapshot.state)},
        {"input", {{"path", request.sourcePath}, {"clip_id", request.clipId}}},
        {"parameters", {
            {"format", formatName(request.format)},
            {"resume", request.resume},
            {"overwrite", request.overwrite}}},
        {"artifacts", {
            {"proxy", snapshot.outputDirectory},
            {"manifest", snapshot.manifestPath}}},
        {"progress", {
            {"frames_completed", snapshot.framesCompleted},
            {"total_frames", snapshot.totalFrames}}},
        {"message", snapshot.status},
    };
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        output << root.dump(2) << '\n';
        if (!output.good()) return false;
    }
    std::error_code error;
    fs::rename(temporary, path, error);
    if (error) {
        fs::remove(temporary, error);
        return false;
    }
    return true;
}

core::SizeI proxySize(core::SizeI source)
{
    if (!source.valid()) return {};
    if (source.width > 1280) {
        source.height = std::max(
            1, static_cast<int>(std::lround(
                static_cast<double>(source.height) * 1280.0 /
                static_cast<double>(source.width))));
        source.width = 1280;
    }
    // JPEG/YUV420 encoders require even dimensions.
    source.width = std::max(2, source.width & ~1);
    source.height = std::max(2, source.height & ~1);
    return source;
}

} // namespace

struct ProxyGenerationJobController::Impl {
    mutable std::mutex mutex;
    std::thread worker;
    std::atomic<bool> cancelRequested{false};
    ProxyGenerationJobSnapshot current;
    ProxyGenerationJobRequest request;

    void publish(ProxyGenerationJobSnapshot::State state,
                 std::string status,
                 std::int64_t completed = -1,
                 std::int64_t total = -1)
    {
        ProxyGenerationJobSnapshot copy;
        {
            std::lock_guard<std::mutex> lock(mutex);
            current.state = state;
            current.status = std::move(status);
            if (completed >= 0) current.framesCompleted = completed;
            if (total >= 0) current.totalFrames = total;
            copy = current;
        }
        (void)writeManifest(copy, request);
    }

    void run()
    {
        const standalone_render::StandaloneMediaInfo media =
            standalone_render::probeStandaloneMedia(request.sourcePath);
        if (!media.probed || !media.hasVideo || !media.frameSize.valid()) {
            publish(ProxyGenerationJobSnapshot::State::Failed,
                    media.message.empty()
                        ? "Proxy source could not be probed as video."
                        : media.message);
            return;
        }

        const fs::path output(request.outputDirectory);
        const std::int64_t sourceDurationFrames =
            std::max<std::int64_t>(1, media.durationFrames);
        std::int64_t resumeFrame = 0;
        std::error_code error;
        if (request.resume) {
            const std::optional<std::int64_t> next =
                imageSequenceResumeFrame(output);
            if (!next.has_value()) {
                publish(ProxyGenerationJobSnapshot::State::Failed,
                        "No generated proxy frames are available to resume.");
                return;
            }
            resumeFrame = *next;
        } else if (fs::exists(output, error)) {
            if (!request.overwrite) {
                publish(ProxyGenerationJobSnapshot::State::Failed,
                        "Proxy output already exists; enable overwrite to regenerate it.");
                return;
            }
            fs::remove_all(output, error);
            if (error) {
                publish(ProxyGenerationJobSnapshot::State::Failed,
                        "Existing proxy output could not be removed.");
                return;
            }
        }

        EditorDocumentCore document;
        document.projectName = "Proxy generation";
        document.tracks.push_back({1, "Proxy source", true});
        EditorClip clip;
        clip.id = request.clipId > 0 ? request.clipId : 1;
        clip.trackId = 1;
        clip.persistentId = "proxy-source";
        clip.sourcePath = request.sourcePath;
        clip.mediaKind = "video";
        clip.sourceFps = media.videoFps > 0.001 ? media.videoFps : 30.0;
        clip.sourceDurationFrames = static_cast<int>(std::min<std::int64_t>(
            sourceDurationFrames, std::numeric_limits<int>::max()));
        clip.durationFrames = std::max(
            1, static_cast<int>(std::ceil(
                static_cast<double>(sourceDurationFrames) * 30.0 /
                clip.sourceFps)));
        clip.hasAudio = false;
        clip.audioPresenceKnown = true;
        clip.audioEnabled = false;
        document.clips.push_back(clip);

        document.exportRequest.outputPath =
            request.format == ProxyGenerationFormat::ImageSequenceJpeg
            ? output.string() + ".render"
            : output.string();
        document.exportRequest.outputSize = proxySize(media.frameSize);
        document.exportRequest.outputFps = clip.sourceFps;
        document.exportRequest.outputMode =
            request.format == ProxyGenerationFormat::ImageSequenceJpeg
            ? render::RenderOutputMode::ImageSequence
            : render::RenderOutputMode::EncodedFile;
        document.exportRequest.imageSequenceFormat =
            request.format == ProxyGenerationFormat::ImageSequenceJpeg
            ? "jpg" : "";
        document.exportRequest.outputFormat =
            request.format == ProxyGenerationFormat::H264Mp4
            ? "mp4"
            : (request.format == ProxyGenerationFormat::MjpegMov
                ? "mov_mjpeg" : "");
        document.exportRequest.exportStartFrame = static_cast<std::int64_t>(
            std::llround(
                static_cast<double>(resumeFrame) * 30.0 /
                clip.sourceFps));
        document.exportRequest.exportEndFrame = clip.durationFrames;

        const std::int64_t framesRemaining =
            std::max<std::int64_t>(0, sourceDurationFrames - resumeFrame);
        if (framesRemaining == 0) {
            publish(ProxyGenerationJobSnapshot::State::Completed,
                    "Proxy already contains all expected frames.",
                    sourceDurationFrames, sourceDurationFrames);
            return;
        }
        const std::string runningMessage =
            request.format == ProxyGenerationFormat::ImageSequenceJpeg
            ? (request.resume
                ? "Continuing JPEG image-sequence proxy generation."
                : "Generating JPEG image-sequence proxy with shared renderer.")
            : (request.format == ProxyGenerationFormat::H264Mp4
                ? "Generating H.264 MP4 proxy with shared renderer."
                : "Generating Motion JPEG MOV proxy with shared renderer.");
        publish(ProxyGenerationJobSnapshot::State::Running,
                runningMessage, resumeFrame, sourceDurationFrames);
        const render::RenderResultCore result =
            standalone_render::exportTimelineToFile(
                {std::move(document), fs::path(request.sourcePath)
                                          .parent_path().string(),
                 resumeFrame,
                 framesRemaining},
                [this, resumeFrame, sourceDurationFrames, runningMessage](
                    const render::RenderProgressCore& progress) {
                    const std::int64_t completed =
                        resumeFrame + progress.framesCompleted;
                    if (cancelRequested.load()) {
                        publish(
                            ProxyGenerationJobSnapshot::State::Canceling,
                            "Canceling proxy generation; completed frames are preserved.",
                            completed, sourceDurationFrames);
                        return false;
                    }
                    publish(
                        ProxyGenerationJobSnapshot::State::Running,
                        runningMessage,
                        completed, sourceDurationFrames);
                    return true;
                });
        if (result.cancelled || cancelRequested.load()) {
            publish(ProxyGenerationJobSnapshot::State::Canceled,
                    "Proxy generation canceled; partial frames were preserved.",
                    resumeFrame + result.framesRendered,
                    sourceDurationFrames);
        } else if (!result.success) {
            publish(ProxyGenerationJobSnapshot::State::Failed,
                    result.message.empty()
                        ? "Proxy generation failed."
                        : result.message,
                    resumeFrame + result.framesRendered,
                    sourceDurationFrames);
        } else {
            publish(ProxyGenerationJobSnapshot::State::Completed,
                    "Proxy generation completed.",
                    resumeFrame + result.framesRendered,
                    sourceDurationFrames);
        }
    }
};

ProxyGenerationJobController::ProxyGenerationJobController()
    : impl_(std::make_unique<Impl>())
{
}

ProxyGenerationJobController::~ProxyGenerationJobController()
{
    cancel();
    wait();
}

bool ProxyGenerationJobController::start(
    const ProxyGenerationJobRequest& request,
    std::string* errorOut)
{
    if (errorOut) errorOut->clear();
    wait();
    if (request.sourcePath.empty() || request.outputDirectory.empty()) {
        if (errorOut) *errorOut = "Proxy job requires source and output paths.";
        return false;
    }
    std::error_code error;
    if (!fs::exists(request.sourcePath, error) || error) {
        if (errorOut) *errorOut = "Proxy source does not exist.";
        return false;
    }
    const fs::path output(request.outputDirectory);
    if (request.outputDirectory !=
        defaultProxyOutputPath(request.sourcePath, request.format)) {
        if (errorOut) *errorOut =
            "Proxy output does not match the selected source-adjacent format.";
        return false;
    }
    if (request.resume &&
        request.format != ProxyGenerationFormat::ImageSequenceJpeg) {
        if (errorOut) {
            *errorOut = "Only image-sequence proxies support resume.";
        }
        return false;
    }
    if (request.resume && request.overwrite) {
        if (errorOut) *errorOut = "Resume and overwrite are mutually exclusive.";
        return false;
    }
    if (fs::exists(output, error) && !error &&
        !request.overwrite && !request.resume) {
        if (errorOut) {
            *errorOut =
                "Proxy output already exists; enable overwrite to regenerate it.";
        }
        return false;
    }
    fs::create_directories(output.parent_path(), error);
    if (error) {
        if (errorOut) *errorOut = "Proxy output parent could not be created.";
        return false;
    }

    impl_->request = request;
    impl_->cancelRequested.store(false);
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->current = {};
        impl_->current.state = ProxyGenerationJobSnapshot::State::Starting;
        impl_->current.clipId = request.clipId;
        impl_->current.status = "Preparing proxy generation.";
        impl_->current.outputDirectory = request.outputDirectory;
        impl_->current.format = request.format;
        impl_->current.manifestPath =
            request.format == ProxyGenerationFormat::ImageSequenceJpeg
            ? (output / "proxy_manifest.json").string()
            : output.string() + ".manifest.json";
    }
    impl_->worker = std::thread([impl = impl_.get()] { impl->run(); });
    return true;
}

std::string defaultProxyOutputPath(
    const std::string& sourcePath,
    ProxyGenerationFormat format)
{
    const fs::path source(sourcePath);
    const fs::path parent = source.parent_path();
    const std::string stem = source.stem().string();
    switch (format) {
    case ProxyGenerationFormat::ImageSequenceJpeg:
        return (parent / (stem + ".proxy")).string();
    case ProxyGenerationFormat::H264Mp4:
        return (parent / (stem + ".proxy.mp4")).string();
    case ProxyGenerationFormat::MjpegMov:
        return (parent / (stem + ".proxy.mov")).string();
    }
    return {};
}

bool removeProxyArtifact(const std::string& sourcePath,
                         const std::string& proxyPath,
                         std::string* errorOut)
{
    if (errorOut) errorOut->clear();
    bool recognized = false;
    for (const ProxyGenerationFormat format : {
             ProxyGenerationFormat::ImageSequenceJpeg,
             ProxyGenerationFormat::H264Mp4,
             ProxyGenerationFormat::MjpegMov}) {
        if (fs::path(proxyPath).lexically_normal() ==
            fs::path(defaultProxyOutputPath(sourcePath, format))
                .lexically_normal()) {
            recognized = true;
            break;
        }
    }
    if (!recognized) {
        if (errorOut) *errorOut =
            "Refusing to delete a path outside the source proxy candidates.";
        return false;
    }
    std::error_code error;
    const fs::file_status status = fs::symlink_status(proxyPath, error);
    if (error || !fs::exists(status)) {
        if (errorOut) *errorOut = "Proxy artifact does not exist.";
        return false;
    }
    if (fs::is_symlink(status)) {
        if (errorOut) *errorOut = "Refusing to delete a proxy symlink.";
        return false;
    }
    if (fs::is_directory(status)) {
        fs::remove_all(proxyPath, error);
    } else if (fs::is_regular_file(status)) {
        fs::remove(proxyPath, error);
        const fs::path manifest = proxyPath + ".manifest.json";
        std::error_code ignored;
        fs::remove(manifest, ignored);
    } else {
        if (errorOut) *errorOut = "Proxy artifact is not a file or directory.";
        return false;
    }
    if (error) {
        if (errorOut) *errorOut = "Proxy artifact could not be deleted.";
        return false;
    }
    return true;
}

void ProxyGenerationJobController::cancel()
{
    impl_->cancelRequested.store(true);
}

ProxyGenerationJobSnapshot ProxyGenerationJobController::snapshot() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->current;
}

void ProxyGenerationJobController::wait()
{
    if (impl_->worker.joinable()) impl_->worker.join();
}

} // namespace jcut
