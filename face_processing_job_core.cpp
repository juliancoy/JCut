#include "face_processing_job_core.h"

#include "face_artifact_core.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

extern char** environ;

namespace jcut {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

std::string safeToken(std::string value)
{
    for (char& character : value) {
        const unsigned char byte = static_cast<unsigned char>(character);
        if (!std::isalnum(byte) && character != '.' && character != '_' &&
            character != '-') {
            character = '_';
        }
    }
    while (value.find("__") != std::string::npos) {
        value.replace(value.find("__"), 2, "_");
    }
    if (value.empty() || value == "." || value == "..") value = "unknown";
    if (value.size() > 96) value.resize(96);
    return value;
}

std::string number(double value)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(4) << value;
    return stream.str();
}

bool writeJsonAtomically(const fs::path& path, const json& value)
{
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    if (error) return false;
    const fs::path temporary =
        path.string() + ".tmp-" + std::to_string(::getpid());
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        output << value.dump(2) << '\n';
        if (!output.good()) return false;
    }
    fs::rename(temporary, path, error);
    if (error) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return false;
    }
    return true;
}

const char* stateName(FaceProcessingJobSnapshot::State state)
{
    using State = FaceProcessingJobSnapshot::State;
    switch (state) {
    case State::Idle: return "idle";
    case State::Starting: return "prepared";
    case State::Running: return "running";
    case State::Canceling: return "canceling";
    case State::Paused: return "paused";
    case State::Completed: return "completed";
    case State::Failed: return "failed";
    }
    return "failed";
}

bool requiredOutputsExist(const fs::path& directory, std::string* missingOut)
{
    const std::vector<std::string> names{
        "continuity_facedetections.bin",
        "detections.idx",
        "tracks.idx",
        "summary.json",
    };
    for (const std::string& name : names) {
        std::error_code error;
        if (!fs::is_regular_file(directory / name, error) || error) {
            if (missingOut) *missingOut = name;
            return false;
        }
    }
    return true;
}

} // namespace

struct FaceProcessingJobController::Impl {
    mutable std::mutex mutex;
    std::thread worker;
    std::atomic<bool> cancelRequested{false};
    FaceProcessingJobSnapshot current;
    FaceProcessingJobRequest request;
    std::vector<std::string> command;

    void publish(FaceProcessingJobSnapshot::State state,
                 std::string status,
                 int processId = -1,
                 int exitCode = -1)
    {
        std::lock_guard<std::mutex> lock(mutex);
        current.state = state;
        current.status = std::move(status);
        if (processId >= 0) current.processId = processId;
        if (exitCode >= 0) current.exitCode = exitCode;

        json manifest{
            {"schema", "jcut_processing_job_v1"},
            {"operation", "facedetections"},
            {"status", stateName(state)},
            {"job_root", current.outputDirectory},
            {"input", {{"path", request.mediaPath}}},
            {"parameters", {
                {"clip_id", request.clipId},
                {"start_frame", request.startFrame},
                {"max_frames", request.maxFrames},
                {"stride", request.stride},
                {"detector_workers", request.detectorWorkers},
                {"detector_pipeline_slots", request.detectorPipelineSlots}}},
            {"artifacts", {
                {"manifest", current.manifestPath},
                {"checkpoint", (fs::path(current.outputDirectory) / "facedetections.part").string()},
                {"resume_index", (fs::path(current.outputDirectory) / "facedetections.part.resume_index.json").string()},
                {"detections", (fs::path(current.outputDirectory) / "detections.idx").string()},
                {"tracks", (fs::path(current.outputDirectory) / "tracks.idx").string()},
                {"continuity", (fs::path(current.outputDirectory) / "continuity_facedetections.bin").string()},
                {"summary", (fs::path(current.outputDirectory) / "summary.json").string()},
                {"log", current.logPath}}},
            {"command", command},
            {"process_id", current.processId},
            {"exit_code", current.exitCode},
            {"message", current.status},
        };
        (void)writeJsonAtomically(current.manifestPath, manifest);
    }

    void run()
    {
        const fs::path logPath(current.logPath);
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(
            &actions, STDOUT_FILENO, logPath.c_str(),
            O_WRONLY | O_CREAT | O_TRUNC, 0644);
        posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);

        std::vector<char*> arguments;
        arguments.reserve(command.size() + 1);
        for (std::string& item : command) arguments.push_back(item.data());
        arguments.push_back(nullptr);

        pid_t child = -1;
        const int spawnResult = posix_spawn(
            &child, command.front().c_str(), &actions, nullptr,
            arguments.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        if (spawnResult != 0) {
            publish(FaceProcessingJobSnapshot::State::Failed,
                    "Could not start face detector (error " +
                        std::to_string(spawnResult) + ").");
            return;
        }
        publish(FaceProcessingJobSnapshot::State::Running,
                "Face detection and continuity generation is running.",
                static_cast<int>(child));

        int status = 0;
        bool termSent = false;
        auto cancelStarted = std::chrono::steady_clock::time_point{};
        while (::waitpid(child, &status, WNOHANG) == 0) {
            if (cancelRequested.load()) {
                if (!termSent) {
                    ::kill(child, SIGTERM);
                    termSent = true;
                    cancelStarted = std::chrono::steady_clock::now();
                    publish(FaceProcessingJobSnapshot::State::Canceling,
                            "Canceling face job; resumable checkpoints will be preserved.");
                } else if (std::chrono::steady_clock::now() - cancelStarted >
                           std::chrono::seconds(3)) {
                    ::kill(child, SIGKILL);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (cancelRequested.load()) {
            publish(FaceProcessingJobSnapshot::State::Paused,
                    "Face job canceled; resumable checkpoints were preserved.",
                    static_cast<int>(child),
                    WIFEXITED(status) ? WEXITSTATUS(status) : 128);
            return;
        }
        const int exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
        if (exitCode != 0) {
            publish(FaceProcessingJobSnapshot::State::Failed,
                    "Face detector failed; inspect the job log.",
                    static_cast<int>(child), exitCode);
            return;
        }
        std::string missing;
        if (!requiredOutputsExist(current.outputDirectory, &missing)) {
            publish(FaceProcessingJobSnapshot::State::Failed,
                    "Face detector did not produce required artifact: " + missing,
                    static_cast<int>(child), exitCode);
            return;
        }
        std::string importError;
        if (!importGeneratedFaceArtifacts(
                current.outputDirectory,
                request.transcriptPath,
                request.clipId,
                &importError)) {
            publish(FaceProcessingJobSnapshot::State::Failed,
                    importError.empty()
                        ? "Face artifacts could not be imported."
                        : importError,
                    static_cast<int>(child), exitCode);
            return;
        }
        publish(FaceProcessingJobSnapshot::State::Completed,
                "Face detection and continuity artifacts were imported.",
                static_cast<int>(child), exitCode);
    }
};

std::string faceProcessingSidecarDirectory(const std::string& mediaPath,
                                           const std::string& clipId)
{
    const fs::path media(mediaPath);
    const std::string stem = safeToken(media.stem().string());
    return (media.parent_path() / (stem + ".jcut") / "facedetections" /
            safeToken(clipId.empty() ? "unknown_clip" : clipId)).string();
}

std::vector<std::string> faceProcessingCommand(
    const FaceProcessingJobRequest& request)
{
    std::vector<std::string> command{
        request.executablePath,
        request.mediaPath,
        "--detector", "scrfd-ncnn-vulkan",
        "--stride", std::to_string(std::max(1, request.stride)),
        "--threshold", number(std::clamp(request.threshold, 0.0, 1.0)),
        "--start-frame", std::to_string(std::max<std::int64_t>(0, request.startFrame)),
        "--quiet", "--no-progress",
        "--detector-workers", std::to_string(std::clamp(request.detectorWorkers, 1, 10)),
        "--detector-pipeline-slots",
        std::to_string(std::clamp(request.detectorPipelineSlots, 1, 10)),
        "--out-dir", request.outputDirectory,
        request.primaryFaceOnly ? "--primary-face-only" : "--multi-face",
        request.smallFaceFallback ? "--small-face-fallback" : "--no-small-face-fallback",
        request.scrfdTiling ? "--scrfd-tiling" : "--no-scrfd-tiling",
        request.allowCpuUploadFallback
            ? "--allow-cpu-upload-fallback" : "--require-zero-copy",
        "--no-control-window", "--no-preview-window", "--no-preview-files",
    };
    if (!request.detectorSettingsPath.empty()) {
        command.insert(command.end(), {
            "--params-file", request.detectorSettingsPath});
    }
    if (request.maxFrames > 0) {
        command.insert(command.end(), {
            "--max-frames", std::to_string(request.maxFrames)});
    }
    return command;
}

FaceProcessingJobController::FaceProcessingJobController()
    : impl_(std::make_unique<Impl>())
{
}

FaceProcessingJobController::~FaceProcessingJobController()
{
    cancel();
    wait();
}

bool FaceProcessingJobController::start(
    const FaceProcessingJobRequest& request,
    std::string* errorOut)
{
    if (errorOut) errorOut->clear();
    wait();
    if (request.executablePath.empty() || request.mediaPath.empty() ||
        request.outputDirectory.empty()) {
        if (errorOut) *errorOut = "Face job requires executable, media, and output paths.";
        return false;
    }
    std::error_code error;
    if (!fs::is_regular_file(request.executablePath, error) || error) {
        if (errorOut) *errorOut = "Face detector executable was not found.";
        return false;
    }
    fs::create_directories(request.outputDirectory, error);
    if (error) {
        if (errorOut) *errorOut = "Could not create the face job directory.";
        return false;
    }
    impl_->request = request;
    impl_->command = faceProcessingCommand(request);
    impl_->cancelRequested.store(false);
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->current = {};
        impl_->current.state = FaceProcessingJobSnapshot::State::Starting;
        impl_->current.status = "Preparing face detection job.";
        impl_->current.outputDirectory = request.outputDirectory;
        impl_->current.manifestPath =
            (fs::path(request.outputDirectory) / "manifest.json").string();
        impl_->current.logPath =
            (fs::path(request.outputDirectory) / "generator.log").string();
    }
    impl_->publish(FaceProcessingJobSnapshot::State::Starting,
                   "Preparing face detection job.");
    impl_->worker = std::thread([impl = impl_.get()] { impl->run(); });
    return true;
}

void FaceProcessingJobController::cancel()
{
    impl_->cancelRequested.store(true);
}

FaceProcessingJobSnapshot FaceProcessingJobController::snapshot() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->current;
}

void FaceProcessingJobController::wait()
{
    if (impl_->worker.joinable()) impl_->worker.join();
}

} // namespace jcut
