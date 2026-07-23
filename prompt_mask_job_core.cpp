#include "prompt_mask_job_core.h"

#include "mask_sidecar_core.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string_view>
#include <thread>
#include <unordered_map>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

extern char** environ;

namespace jcut::masks {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

std::string utcNow()
{
    const std::time_t now = std::time(nullptr);
    std::tm value{};
    gmtime_r(&now, &value);
    std::ostringstream stream;
    stream << std::put_time(&value, "%Y-%m-%dT%H:%M:%SZ");
    return stream.str();
}

bool writeJsonAtomically(const fs::path& path,
                         const json& value,
                         std::string* errorOut)
{
    if (errorOut) errorOut->clear();
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    if (error) {
        if (errorOut) *errorOut = "Could not create prompt-mask job directory.";
        return false;
    }
    const fs::path temporary =
        path.string() + ".tmp-" + std::to_string(::getpid());
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            if (errorOut) *errorOut = "Could not open prompt-mask manifest.";
            return false;
        }
        output << value.dump(2) << '\n';
        if (!output.good()) {
            if (errorOut) *errorOut = "Could not write prompt-mask manifest.";
            return false;
        }
    }
    fs::rename(temporary, path, error);
    if (error) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        if (errorOut) *errorOut = "Could not commit prompt-mask manifest.";
        return false;
    }
    return true;
}

const char* stateName(PromptMaskJobSnapshot::State state)
{
    using State = PromptMaskJobSnapshot::State;
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

std::vector<std::string> environmentFor(
    const PromptMaskJobRequest& request,
    const PromptMaskJobPlan& plan)
{
    std::unordered_map<std::string, std::string> values;
    for (char** item = environ; item && *item; ++item) {
        const std::string entry(*item);
        const std::size_t separator = entry.find('=');
        if (separator != std::string::npos) {
            values[entry.substr(0, separator)] = entry.substr(separator + 1);
        }
    }
    values["SAM3_MODEL_CACHE"] = request.modelCachePath;
    values["SAM3_RUNTIME_CACHE"] = request.runtimeCachePath;
    values["SAM3_JOB_DIR"] = plan.jobRoot;
    if (request.runDockerAsRoot) {
        values["SAM3_DOCKER_RUN_AS_ROOT"] = "1";
    } else {
        values.erase("SAM3_DOCKER_RUN_AS_ROOT");
    }
    std::vector<std::string> result;
    result.reserve(values.size());
    for (const auto& [name, value] : values) {
        result.push_back(name + "=" + value);
    }
    return result;
}

} // namespace

std::string sanitizedPromptMaskJobComponent(std::string value)
{
    const auto notSpace = [](unsigned char byte) {
        return !std::isspace(byte);
    };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(),
                value.end());
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
    if (value.empty() || value == "." || value == "..") value = "job";
    if (value.size() > 96) value.resize(96);
    return value;
}

PromptMaskJobPlan buildPromptMaskJobPlan(
    const PromptMaskJobRequest& request)
{
    PromptMaskJobPlan plan;
    const fs::path media = fs::absolute(fs::path(request.mediaPath));
    const std::string mediaStem = media.stem().string();
    const std::string stem =
        sanitizedPromptMaskJobComponent(mediaStem);
    const std::string prompt =
        sanitizedPromptMaskJobComponent(request.prompt);
    plan.jobRoot = (
        media.parent_path() / ".jcut_jobs" /
        ("sam3_" + prompt + "_" + stem)).string();
    plan.manifestPath = (fs::path(plan.jobRoot) / "manifest.json").string();
    const std::string outputStem = mediaStem + "_sam3_" + prompt;
    plan.centersPath =
        (media.parent_path() / (outputStem + ".jsonl")).string();
    plan.binaryMasksPath =
        (media.parent_path() / (outputStem + "_binary_masks")).string();

    std::string currentComponent =
        fs::path(request.currentMaskDirectory).filename().string();
    const std::string generatedPrefix = mediaStem + "_sam3_";
    if (currentComponent.starts_with(generatedPrefix)) {
        currentComponent.erase(0, generatedPrefix.size());
    }
    constexpr std::string_view suffix("_binary_masks");
    if (currentComponent.ends_with(suffix)) {
        currentComponent.resize(currentComponent.size() - suffix.size());
    }
    if (request.unionWithCurrentMask && !request.currentMaskDirectory.empty()) {
        plan.combinedMasksPath = (
            media.parent_path() /
            (mediaStem + "_sam3_" +
             sanitizedPromptMaskJobComponent(currentComponent) + "_or_" +
             prompt + "_binary_masks")).string();
    }
    plan.selectedMaskPath = request.writeBinaryMasks
        ? (!plan.combinedMasksPath.empty()
               ? plan.combinedMasksPath
               : plan.binaryMasksPath)
        : std::string{};
    plan.defaultOutputPath =
        (media.parent_path() / (outputStem + ".mp4")).string();
    plan.logPath = (fs::path(plan.jobRoot) / "job.log").string();

    plan.command = {
        request.scriptPath,
        media.string(),
        "--prompt",
        request.prompt,
    };
    if (request.videoMode) plan.command.push_back("--video-mode");
    if (request.writeBinaryMasks && !request.videoMode) {
        plan.command.insert(plan.command.end(), {
            "--binary-mask-dir", plan.binaryMasksPath});
    }
    if (request.unionWithCurrentMask && !request.videoMode &&
        !request.currentMaskDirectory.empty()) {
        plan.command.insert(plan.command.end(), {
            "--union-mask-dir", request.currentMaskDirectory,
            "--combined-binary-mask-dir", plan.combinedMasksPath});
    }
    if (request.writeMaskPreviewFrames && !request.videoMode) {
        plan.command.push_back("--write-mask-preview-frames");
    }
    if (request.exportCentersJson) {
        plan.command.insert(plan.command.end(), {
            "--centers-json", plan.centersPath});
    } else {
        plan.command.push_back("--no-centers-json");
    }
    if (request.scaleWidth > 0) {
        plan.command.insert(plan.command.end(), {
            "--scale-width", std::to_string(
                std::clamp(request.scaleWidth, 1, 8192))});
    }
    if (request.prescaleWidth > 0) {
        plan.command.insert(plan.command.end(), {
            "--prescale-width", std::to_string(
                std::clamp(request.prescaleWidth, 1, 8192))});
    }
    if (!request.videoMode && !request.writeBinaryMasks &&
        std::isfinite(request.extractFps) && request.extractFps > 0.0) {
        std::ostringstream fps;
        fps << std::fixed << std::setprecision(3)
            << std::clamp(request.extractFps, 0.0, 240.0);
        plan.command.insert(plan.command.end(), {"--extract-fps", fps.str()});
    }
    if (!request.videoMode &&
        request.intermediateFramesFormat == "png") {
        plan.command.insert(plan.command.end(), {
            "--intermediate-frames-format", "png"});
    }
    if (request.compileModel) plan.command.push_back("--compile-model");

    const std::string now = utcNow();
    plan.manifest = {
        {"schema", "jcut_processing_job_v1"},
        {"operation", "sam3"},
        {"status", "prepared"},
        {"created_at_utc", now},
        {"updated_at_utc", now},
        {"job_root", plan.jobRoot},
        {"input", {{"path", media.string()}, {"exists", fs::exists(media)}}},
        {"parameters", {
            {"prompt", request.prompt},
            {"video_mode", request.videoMode},
            {"extract_frames", !request.videoMode},
            {"stream_extract", !request.videoMode},
            {"binary_masks", request.writeBinaryMasks && !request.videoMode},
            {"union_with_current_mask", request.unionWithCurrentMask},
            {"union_mask_dir", request.currentMaskDirectory},
            {"mask_preview_frames", request.writeMaskPreviewFrames},
            {"centers_json", request.exportCentersJson},
            {"docker_root_mode", request.runDockerAsRoot},
            {"scale_width", request.scaleWidth},
            {"prescale_width", request.prescaleWidth},
            {"extract_fps", request.extractFps},
            {"intermediate_frames_format", request.intermediateFramesFormat},
            {"compile_model", request.compileModel}}},
        {"artifacts", {
            {"job_root", plan.jobRoot},
            {"manifest", plan.manifestPath},
            {"centers_json",
             request.exportCentersJson ? plan.centersPath : std::string{}},
            {"binary_masks_dir",
             request.writeBinaryMasks ? plan.binaryMasksPath : std::string{}},
            {"combined_binary_masks_dir", plan.combinedMasksPath},
            {"default_output_video", plan.defaultOutputPath},
            {"model_cache", request.modelCachePath},
            {"runtime_cache", request.runtimeCachePath},
            {"log", plan.logPath}}},
        {"command", plan.command},
    };
    return plan;
}

bool writePromptMaskJobManifest(
    const PromptMaskJobPlan& plan,
    std::string* errorOut)
{
    return writeJsonAtomically(plan.manifestPath, plan.manifest, errorOut);
}

struct PromptMaskJobController::Impl {
    mutable std::mutex mutex;
    std::thread worker;
    std::atomic<bool> cancelRequested{false};
    PromptMaskJobRequest request;
    PromptMaskJobPlan plan;
    PromptMaskJobSnapshot current;

    void publish(PromptMaskJobSnapshot::State state,
                 std::string status,
                 int processId = -1,
                 int exitCode = -1)
    {
        PromptMaskJobSnapshot copy;
        {
            std::lock_guard<std::mutex> lock(mutex);
            current.state = state;
            current.status = std::move(status);
            if (processId >= 0) current.processId = processId;
            if (exitCode >= 0) current.exitCode = exitCode;
            copy = current;
        }
        json manifest = plan.manifest;
        std::ifstream input(plan.manifestPath, std::ios::binary);
        if (input) {
            try {
                manifest = json::parse(input);
            } catch (...) {
            }
        }
        manifest["status"] = stateName(state);
        manifest["updated_at_utc"] = utcNow();
        manifest["process_id"] = copy.processId;
        manifest["exit_code"] = copy.exitCode;
        manifest["message"] = copy.status;
        (void)writeJsonAtomically(plan.manifestPath, manifest, nullptr);
    }

    void run()
    {
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(
            &actions, STDOUT_FILENO, plan.logPath.c_str(),
            O_WRONLY | O_CREAT | O_TRUNC, 0644);
        posix_spawn_file_actions_adddup2(
            &actions, STDOUT_FILENO, STDERR_FILENO);

        std::vector<char*> arguments;
        arguments.reserve(plan.command.size() + 1);
        for (std::string& value : plan.command) {
            arguments.push_back(value.data());
        }
        arguments.push_back(nullptr);
        std::vector<std::string> environment =
            environmentFor(request, plan);
        std::vector<char*> environmentPointers;
        environmentPointers.reserve(environment.size() + 1);
        for (std::string& value : environment) {
            environmentPointers.push_back(value.data());
        }
        environmentPointers.push_back(nullptr);

        pid_t child = -1;
        const int spawnResult = posix_spawn(
            &child, plan.command.front().c_str(), &actions, nullptr,
            arguments.data(), environmentPointers.data());
        posix_spawn_file_actions_destroy(&actions);
        if (spawnResult != 0) {
            publish(PromptMaskJobSnapshot::State::Failed,
                    "Could not start SAM3 prompt-mask job (error " +
                        std::to_string(spawnResult) + ").");
            return;
        }
        publish(PromptMaskJobSnapshot::State::Running,
                "SAM3 prompt-mask generation is running.",
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
                    publish(PromptMaskJobSnapshot::State::Canceling,
                            "Canceling SAM3; resumable outputs are preserved.");
                } else if (
                    std::chrono::steady_clock::now() - cancelStarted >
                    std::chrono::seconds(3)) {
                    ::kill(child, SIGKILL);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        const int exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
        if (cancelRequested.load()) {
            publish(PromptMaskJobSnapshot::State::Paused,
                    "SAM3 canceled; use Resume to continue existing outputs.",
                    static_cast<int>(child), exitCode);
            return;
        }
        if (exitCode != 0) {
            publish(PromptMaskJobSnapshot::State::Failed,
                    "SAM3 failed; inspect the prompt-mask job log.",
                    static_cast<int>(child), exitCode);
            return;
        }
        if (!plan.selectedMaskPath.empty()) {
            const MaskSidecarCore sidecar =
                inspectMaskSidecarCore(plan.selectedMaskPath, request.mediaPath);
            if (!sidecar.valid()) {
                publish(PromptMaskJobSnapshot::State::Failed,
                        "SAM3 completed without producing binary mask frames.",
                        static_cast<int>(child), exitCode);
                return;
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                current.selectedMaskId = sidecar.id;
                current.selectedMaskName = sidecar.displayName;
            }
        }
        publish(PromptMaskJobSnapshot::State::Completed,
                plan.selectedMaskPath.empty()
                    ? "SAM3 prompt-mask job completed."
                    : "SAM3 masks completed and are ready to materialize.",
                static_cast<int>(child), exitCode);
    }
};

PromptMaskJobController::PromptMaskJobController()
    : impl_(std::make_unique<Impl>())
{
}

PromptMaskJobController::~PromptMaskJobController()
{
    cancel();
    wait();
}

bool PromptMaskJobController::start(
    const PromptMaskJobRequest& request,
    std::string* errorOut)
{
    if (errorOut) errorOut->clear();
    wait();
    if (request.scriptPath.empty() || request.mediaPath.empty() ||
        request.prompt.empty() || request.modelCachePath.empty() ||
        request.runtimeCachePath.empty()) {
        if (errorOut) {
            *errorOut =
                "Prompt-mask job requires script, media, prompt, and cache paths.";
        }
        return false;
    }
    std::error_code error;
    if (!fs::is_regular_file(request.scriptPath, error) || error) {
        if (errorOut) *errorOut = "sam3.sh was not found.";
        return false;
    }
    if (!fs::is_regular_file(request.mediaPath, error) || error) {
        if (errorOut) *errorOut = "Prompt-mask source video was not found.";
        return false;
    }
    if (request.videoMode && request.writeBinaryMasks) {
        if (errorOut) *errorOut = "Video mode cannot write timeline binary masks.";
        return false;
    }
    if (request.unionWithCurrentMask &&
        (!request.writeBinaryMasks || request.currentMaskDirectory.empty())) {
        if (errorOut) {
            *errorOut =
                "Mask union requires binary output and an existing mask directory.";
        }
        return false;
    }
    fs::create_directories(request.modelCachePath, error);
    if (error) {
        if (errorOut) *errorOut = "Could not create SAM3 model cache.";
        return false;
    }
    fs::create_directories(request.runtimeCachePath, error);
    if (error) {
        if (errorOut) *errorOut = "Could not create SAM3 runtime cache.";
        return false;
    }

    PromptMaskJobPlan plan = buildPromptMaskJobPlan(request);
    if (request.restartPolicy == PromptMaskRestartPolicy::Restart) {
        for (const std::string& path : {
                 plan.binaryMasksPath,
                 plan.combinedMasksPath}) {
            if (path.empty()) continue;
            fs::remove_all(path, error);
            if (error) {
                if (errorOut) *errorOut = "Could not clear prior SAM3 output.";
                return false;
            }
        }
        if (request.exportCentersJson) {
            fs::remove(plan.centersPath, error);
            if (error) {
                if (errorOut) *errorOut = "Could not clear prior centers output.";
                return false;
            }
        }
        fs::remove(plan.manifestPath, error);
        if (error) {
            if (errorOut) *errorOut = "Could not clear prior SAM3 manifest.";
            return false;
        }
    }
    fs::create_directories(plan.jobRoot, error);
    if (error) {
        if (errorOut) *errorOut = "Could not create SAM3 job directory.";
        return false;
    }
    plan.manifest["status"] = "running";
    if (!writePromptMaskJobManifest(plan, errorOut)) return false;

    impl_->request = request;
    impl_->plan = std::move(plan);
    impl_->cancelRequested.store(false);
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->current = {};
        impl_->current.state = PromptMaskJobSnapshot::State::Starting;
        impl_->current.status = "Preparing SAM3 prompt-mask generation.";
        impl_->current.jobRoot = impl_->plan.jobRoot;
        impl_->current.manifestPath = impl_->plan.manifestPath;
        impl_->current.selectedMaskPath = impl_->plan.selectedMaskPath;
        impl_->current.logPath = impl_->plan.logPath;
    }
    impl_->worker = std::thread([impl = impl_.get()] { impl->run(); });
    return true;
}

void PromptMaskJobController::cancel()
{
    if (impl_) impl_->cancelRequested.store(true);
}

PromptMaskJobSnapshot PromptMaskJobController::snapshot() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->current;
}

void PromptMaskJobController::wait()
{
    if (impl_ && impl_->worker.joinable()) impl_->worker.join();
}

} // namespace jcut::masks
