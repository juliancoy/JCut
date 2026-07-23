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
#include <iterator>
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

void setArgumentValue(std::vector<std::string>* arguments,
                      const std::string& name,
                      const std::string& value)
{
    if (!arguments) return;
    const auto position = std::find(arguments->begin(), arguments->end(), name);
    if (position != arguments->end() &&
        std::next(position) != arguments->end()) {
        *std::next(position) = value;
        return;
    }
    arguments->push_back(name);
    arguments->push_back(value);
}

std::string benchmarkSlotCsv(const std::vector<int>& configured)
{
    std::vector<int> slots;
    for (const int value : configured) {
        const int clamped = std::clamp(value, 1, 10);
        if (std::find(slots.begin(), slots.end(), clamped) == slots.end()) {
            slots.push_back(clamped);
        }
    }
    if (slots.empty()) slots = {1, 2, 4, 8};
    std::ostringstream stream;
    for (std::size_t index = 0; index < slots.size(); ++index) {
        if (index > 0) stream << ',';
        stream << slots[index];
    }
    return stream.str();
}

bool readJsonObject(const fs::path& path, json* valueOut)
{
    if (!valueOut) return false;
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    try {
        json parsed = json::parse(input);
        if (!parsed.is_object()) return false;
        *valueOut = std::move(parsed);
        return true;
    } catch (const json::exception&) {
        return false;
    }
}

bool parsePipelineBenchmarkLog(const fs::path& path,
                               json* benchmarkOut,
                               std::string* errorOut)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        if (errorOut) *errorOut = "benchmark log could not be read";
        return false;
    }
    const std::string output(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    const std::size_t prefix = output.find("pipeline_benchmark ");
    const std::size_t objectStart =
        output.find('{', prefix == std::string::npos ? 0 : prefix);
    const std::size_t objectEnd = output.rfind('}');
    if (objectStart == std::string::npos ||
        objectEnd == std::string::npos ||
        objectEnd <= objectStart) {
        if (errorOut) {
            *errorOut = "benchmark output did not contain a JSON result";
        }
        return false;
    }
    try {
        json benchmark = json::parse(
            output.substr(objectStart, objectEnd - objectStart + 1));
        if (!benchmark.is_object()) {
            if (errorOut) *errorOut = "benchmark result was not an object";
            return false;
        }
        const int workers =
            benchmark.value("best_detector_workers", -1);
        const int slots =
            benchmark.value("best_detector_pipeline_slots", -1);
        if (workers < 1 || slots < 1) {
            if (errorOut) {
                *errorOut =
                    "benchmark did not identify a valid worker/slot topology";
            }
            return false;
        }
        if (benchmarkOut) *benchmarkOut = std::move(benchmark);
        return true;
    } catch (const json::exception& exception) {
        if (errorOut) {
            *errorOut =
                std::string("benchmark JSON could not be parsed: ") +
                exception.what();
        }
        return false;
    }
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
                {"detector_pipeline_slots", request.detectorPipelineSlots},
                {"control_window", request.controlWindow},
                {"live_preview", request.livePreview},
                {"restart_from_scratch", request.restartFromScratch},
                {"apply_clip_grading", request.applyClipGrading}}},
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
        if (request.benchmarkTopology) {
            publish(FaceProcessingJobSnapshot::State::Starting,
                    "Benchmarking face-detector worker topology.");
            std::vector<std::string> benchmarkCommand = command;
            benchmarkCommand.push_back("--benchmark-pipeline-slots");
            benchmarkCommand.push_back(
                benchmarkSlotCsv(request.benchmarkPipelineSlots));
            const int benchmarkFrames = std::min(
                std::clamp(request.benchmarkFrames, 60, 5000),
                request.maxFrames > 0
                    ? static_cast<int>(std::min<std::int64_t>(
                          request.maxFrames, 5000))
                    : 5000);
            setArgumentValue(
                &benchmarkCommand, "--max-frames",
                std::to_string(std::max(1, benchmarkFrames)));

            const fs::path benchmarkLog =
                fs::path(current.outputDirectory) / "pipeline_benchmark.log";
            posix_spawn_file_actions_t benchmarkActions;
            posix_spawn_file_actions_init(&benchmarkActions);
            posix_spawn_file_actions_addopen(
                &benchmarkActions, STDOUT_FILENO, benchmarkLog.c_str(),
                O_WRONLY | O_CREAT | O_TRUNC, 0644);
            posix_spawn_file_actions_adddup2(
                &benchmarkActions, STDOUT_FILENO, STDERR_FILENO);
            std::vector<char*> arguments;
            arguments.reserve(benchmarkCommand.size() + 1);
            for (std::string& item : benchmarkCommand) {
                arguments.push_back(item.data());
            }
            arguments.push_back(nullptr);
            pid_t benchmarkChild = -1;
            const int spawnResult = posix_spawn(
                &benchmarkChild, benchmarkCommand.front().c_str(),
                &benchmarkActions, nullptr, arguments.data(), environ);
            posix_spawn_file_actions_destroy(&benchmarkActions);
            if (spawnResult != 0) {
                publish(FaceProcessingJobSnapshot::State::Failed,
                        "Could not start face topology benchmark (error " +
                            std::to_string(spawnResult) + ").");
                return;
            }
            int benchmarkStatus = 0;
            bool termSent = false;
            auto cancelStarted = std::chrono::steady_clock::time_point{};
            while (::waitpid(
                       benchmarkChild, &benchmarkStatus, WNOHANG) == 0) {
                if (cancelRequested.load()) {
                    if (!termSent) {
                        ::kill(benchmarkChild, SIGTERM);
                        termSent = true;
                        cancelStarted = std::chrono::steady_clock::now();
                    } else if (
                        std::chrono::steady_clock::now() - cancelStarted >
                        std::chrono::seconds(3)) {
                        ::kill(benchmarkChild, SIGKILL);
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (cancelRequested.load()) {
                publish(FaceProcessingJobSnapshot::State::Paused,
                        "Face topology benchmark canceled.");
                return;
            }
            const int benchmarkExitCode = WIFEXITED(benchmarkStatus)
                ? WEXITSTATUS(benchmarkStatus) : 128;
            if (benchmarkExitCode != 0) {
                publish(FaceProcessingJobSnapshot::State::Failed,
                        "Face topology benchmark failed; inspect pipeline_benchmark.log.",
                        static_cast<int>(benchmarkChild), benchmarkExitCode);
                return;
            }
            json benchmark;
            std::string benchmarkError;
            if (!parsePipelineBenchmarkLog(
                    benchmarkLog, &benchmark, &benchmarkError)) {
                publish(FaceProcessingJobSnapshot::State::Failed,
                        benchmarkError);
                return;
            }
            request.detectorWorkers = std::clamp(
                benchmark.value("best_detector_workers", 2), 1, 10);
            request.detectorPipelineSlots = std::clamp(
                benchmark.value(
                    "best_detector_pipeline_slots", 2), 1, 10);
            command = faceProcessingCommand(request);
            const json launchControl{
                {"schema", "jcut_facedetections_launch_control_v1"},
                {"mode", "auto"},
                {"detector_workers", request.detectorWorkers},
                {"detector_pipeline_slots",
                 request.detectorPipelineSlots},
                {"benchmark_frames",
                 std::clamp(request.benchmarkFrames, 60, 5000)},
                {"benchmark_pipeline_slots",
                 request.benchmarkPipelineSlots},
                {"last_benchmark", benchmark},
            };
            if (!writeJsonAtomically(
                    fs::path(current.outputDirectory) /
                        "launch_control.json",
                    launchControl)) {
                publish(FaceProcessingJobSnapshot::State::Failed,
                        "Face topology benchmark succeeded, but its launch control could not be saved.");
                return;
            }
            publish(
                FaceProcessingJobSnapshot::State::Starting,
                "Topology benchmark selected " +
                    std::to_string(request.detectorWorkers) +
                    " worker(s) and " +
                    std::to_string(request.detectorPipelineSlots) +
                    " pipeline slot(s).");
        }

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
        request.controlWindow ? "--control-window" : "--no-control-window",
        request.livePreview ? "--preview-window" : "--no-preview-window",
        "--no-preview-files",
    };
    if (!request.detectorSettingsPath.empty()) {
        command.insert(command.end(), {
            "--params-file", request.detectorSettingsPath});
    }
    if (request.maxFrames > 0) {
        command.insert(command.end(), {
            "--max-frames", std::to_string(request.maxFrames)});
    }
    if (request.applyClipGrading && !request.clipJsonPath.empty()) {
        command.insert(command.end(), {
            "--clip-json", request.clipJsonPath,
            "--apply-clip-grading"});
    }
    return command;
}

FaceProcessingLaunchControl loadFaceProcessingLaunchControl(
    const std::string& outputDirectory)
{
    FaceProcessingLaunchControl result;
    json control;
    const fs::path path =
        fs::path(outputDirectory) / "launch_control.json";
    if (!fs::exists(path)) return result;
    if (!readJsonObject(path, &control)) {
        result.error = "Saved face launch control is not valid JSON.";
        return result;
    }
    const json benchmark = control.value("last_benchmark", json::object());
    const int workers = benchmark.value(
        "best_detector_workers",
        control.value("detector_workers", -1));
    const int slots = benchmark.value(
        "best_detector_pipeline_slots",
        control.value("detector_pipeline_slots", -1));
    if (workers < 1 || slots < 1) {
        result.error =
            "Saved face launch control has no valid topology recommendation.";
        return result;
    }
    result.hasRecommendation = true;
    result.detectorWorkers = std::clamp(workers, 1, 10);
    result.detectorPipelineSlots = std::clamp(slots, 1, 10);
    result.benchmarkJson = benchmark.dump();
    return result;
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
    if (request.restartFromScratch) {
        for (const char* name :
             {"facedetections.part",
              "facedetections.part.resume_index.json"}) {
            error.clear();
            fs::remove(fs::path(request.outputDirectory) / name, error);
            if (error) {
                if (errorOut) {
                    *errorOut =
                        "Could not clear the face job resume checkpoint.";
                }
                return false;
            }
        }
    }
    FaceProcessingJobRequest prepared = request;
    if (prepared.applyClipGrading) {
        if (prepared.clipJson.empty()) {
            if (errorOut) {
                *errorOut =
                    "Clip grading was requested without clip JSON.";
            }
            return false;
        }
        json parsedClip;
        try {
            parsedClip = json::parse(prepared.clipJson);
        } catch (const json::exception& exception) {
            if (errorOut) {
                *errorOut =
                    std::string("Clip grading JSON is invalid: ") +
                    exception.what();
            }
            return false;
        }
        if (!parsedClip.is_object()) {
            if (errorOut) {
                *errorOut = "Clip grading JSON must be an object.";
            }
            return false;
        }
        prepared.clipJsonPath =
            (fs::path(prepared.outputDirectory) /
             "clip_input.json").string();
        if (!writeJsonAtomically(
                prepared.clipJsonPath, parsedClip)) {
            if (errorOut) {
                *errorOut =
                    "Could not write the face-detector clip grading input.";
            }
            return false;
        }
    } else {
        prepared.clipJsonPath.clear();
    }
    impl_->request = std::move(prepared);
    impl_->command = faceProcessingCommand(impl_->request);
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
