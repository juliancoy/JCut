#include "birefnet_job_core.h"

#include "prompt_mask_job_core.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>

namespace jcut::jobs {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

std::string lowerSlug(std::string value)
{
    for (char& character : value) {
        if (character >= 'A' && character <= 'Z') {
            character =
                static_cast<char>(character - 'A' + 'a');
        } else if (!((character >= 'a' && character <= 'z') ||
                     (character >= '0' && character <= '9'))) {
            character = '-';
        }
    }
    value.erase(
        std::unique(value.begin(), value.end(),
                    [](char left, char right) {
                        return left == '-' && right == '-';
                    }),
        value.end());
    while (!value.empty() && value.front() == '-') {
        value.erase(value.begin());
    }
    while (!value.empty() && value.back() == '-') {
        value.pop_back();
    }
    return value.empty() ? "job" : value;
}

std::string stableContainerName(const fs::path& jobRoot)
{
    // Deterministic FNV-1a suffix. The manifest also records the job-root
    // Docker label, so either frontend can recover the container by identity.
    std::uint64_t digest = 1469598103934665603ULL;
    const std::string absolute = fs::absolute(jobRoot).string();
    for (const unsigned char byte : absolute) {
        digest ^= byte;
        digest *= 1099511628211ULL;
    }
    std::ostringstream suffix;
    suffix << std::hex << std::setw(10) << std::setfill('0')
           << (digest & 0xffffffffffULL);
    std::string readable = lowerSlug(jobRoot.filename().string());
    if (readable.starts_with("birefnet-")) {
        readable.erase(0, 10);
    }
    if (readable.size() > 72) readable.resize(72);
    return "jcut-birefnet-" + readable + "-" + suffix.str();
}

const char* stateName(ProcessJobSnapshotCore::State state)
{
    using State = ProcessJobSnapshotCore::State;
    switch (state) {
    case State::Idle: return "idle";
    case State::Starting: return "starting";
    case State::Running: return "running";
    case State::Canceling: return "canceling";
    case State::Completed: return "completed";
    case State::Canceled: return "paused";
    case State::Failed: return "failed";
    }
    return "failed";
}

bool writeManifest(
    const BiRefNetJobPlanCore& plan,
    const BiRefNetJobSnapshotCore& snapshot)
{
    json manifest = plan.manifest;
    manifest["status"] = stateName(snapshot.state);
    manifest["process_id"] = snapshot.processId;
    manifest["exit_code"] = snapshot.exitCode;
    manifest["message"] = snapshot.status;
    manifest["progress"] = {
        {"current_frame", snapshot.currentFrame},
        {"total_frames", snapshot.totalFrames},
        {"percent", snapshot.percent}};
    manifest["artifacts"]["alpha_ready"] =
        snapshot.outputReady;
    const fs::path path(plan.manifestPath);
    const fs::path temporary = path.string() + ".tmp";
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    if (error) return false;
    {
        std::ofstream output(
            temporary, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        output << manifest.dump(2) << '\n';
        if (!output.good()) return false;
    }
    fs::rename(temporary, path, error);
    if (error) {
        fs::remove(temporary, error);
        return false;
    }
    return true;
}

} // namespace

BiRefNetJobPlanCore buildBiRefNetJobPlanCore(
    const BiRefNetJobRequestCore& request)
{
    BiRefNetJobPlanCore plan;
    const double alphaTolerance = std::clamp(
        std::isfinite(request.alphaTolerance)
            ? request.alphaTolerance
            : 0.0,
        0.0,
        0.99);
    const fs::path script = fs::absolute(request.scriptPath);
    const fs::path media = fs::absolute(request.mediaPath);
    const std::string stem =
        jcut::masks::sanitizedPromptMaskJobComponent(
            media.stem().string());
    const std::string modelName =
        jcut::masks::sanitizedPromptMaskJobComponent(
            fs::path(request.model).filename().string());
    plan.jobRoot = (
        media.parent_path() / ".jcut_jobs" /
        ("birefnet_" + modelName + "_" + stem)).string();
    plan.manifestPath =
        (fs::path(plan.jobRoot) / "manifest.json").string();
    plan.logPath =
        (fs::path(plan.jobRoot) / "job.log").string();
    plan.progressPath =
        (fs::path(plan.jobRoot) / "progress.json").string();
    plan.outputDirectory = request.outputDirectory.empty()
        ? (media.parent_path() /
           (media.stem().string() +
            "_birefnet_alpha_masks")).string()
        : fs::absolute(request.outputDirectory).string();
    plan.livePreviewPath =
        (fs::path(plan.outputDirectory) /
         "jcut_live_preview.png").string();
    plan.containerName = stableContainerName(plan.jobRoot);
    std::ostringstream tolerance;
    tolerance << std::fixed << std::setprecision(4)
              << alphaTolerance;
    plan.command = {
        "/bin/bash",
        script.string(),
        media.string(),
        "--output-dir",
        plan.outputDirectory,
        "--model",
        request.model,
        "--revision",
        request.revision,
        "--alpha-tolerance",
        tolerance.str(),
        "--live-preview",
        "--live-preview-every",
        "1",
        "--progress-every",
        "1",
    };
    if (request.device == "cpu") {
        plan.command.push_back("--cpu");
    }
    if (!request.fp16) plan.command.push_back("--fp32");
    if (request.restart) plan.command.push_back("--no-resume");
    plan.environment = {
        {"BIREFNET_MODEL_CACHE", request.modelCachePath},
        {"BIREFNET_RUNTIME_CACHE", request.runtimeCachePath},
        {"BIREFNET_CONTAINER_NAME", plan.containerName},
        {"BIREFNET_JOB_ROOT", plan.jobRoot},
    };
    if (request.runDockerAsRoot) {
        plan.environment["BIREFNET_DOCKER_RUN_AS_ROOT"] = "1";
    }
    plan.manifest = {
        {"schema", "jcut_processing_job_v1"},
        {"operation", "birefnet"},
        {"status", "prepared"},
        {"job_root", plan.jobRoot},
        {"input", {
            {"path", media.string()},
            {"clip_id", request.clipId}}},
        {"parameters", {
            {"model", request.model},
            {"revision", request.revision},
            {"device", request.device},
            {"fp16", request.fp16},
            {"alpha_tolerance", alphaTolerance},
            {"restart", request.restart},
            {"docker_root_mode", request.runDockerAsRoot},
            {"live_preview", true},
            {"create_mask_marker", true}}},
        {"artifacts", {
            {"alpha_masks_dir", plan.outputDirectory},
            {"model_cache", request.modelCachePath},
            {"runtime_cache", request.runtimeCachePath},
            {"job_log", plan.logPath},
            {"progress", plan.progressPath},
            {"live_preview", plan.livePreviewPath}}},
        {"process", {
            {"type", "docker"},
            {"docker", {
                {"container_name", plan.containerName},
                {"image", "jcut-birefnet:cu126"}}}}},
        {"docker_container_name", plan.containerName},
        {"command", plan.command},
    };
    return plan;
}

struct BiRefNetJobControllerCore::Impl {
    std::mutex mutex;
    ProcessJobControllerCore process;
    BiRefNetJobRequestCore request;
    BiRefNetJobPlanCore plan;
    ProcessJobSnapshotCore::State lastManifestState =
        ProcessJobSnapshotCore::State::Idle;

    BiRefNetJobSnapshotCore snapshot()
    {
        const ProcessJobSnapshotCore processSnapshot =
            process.snapshot();
        BiRefNetJobSnapshotCore result;
        {
            std::lock_guard<std::mutex> lock(mutex);
            result.clipId = request.clipId;
            result.jobRoot = plan.jobRoot;
            result.manifestPath = plan.manifestPath;
            result.logPath = plan.logPath;
            result.progressPath = plan.progressPath;
            result.livePreviewPath = plan.livePreviewPath;
            result.outputDirectory = plan.outputDirectory;
        }
        result.state = processSnapshot.state;
        result.processId = processSnapshot.processId;
        result.exitCode = processSnapshot.exitCode;
        result.status = processSnapshot.status;
        std::ifstream progress(result.progressPath);
        if (progress) {
            try {
                const json parsed = json::parse(progress);
                result.currentFrame =
                    parsed.value("current_frame", 0LL);
                result.totalFrames =
                    parsed.value("total_frames", 0LL);
                result.percent =
                    parsed.value("percent", 0.0);
            } catch (...) {
            }
        }
        std::error_code error;
        result.outputReady = fs::is_regular_file(
            fs::path(result.outputDirectory) / "jcut_alpha.json",
            error) && !error;
        if (result.state != lastManifestState ||
            result.active()) {
            lastManifestState = result.state;
            (void)writeManifest(plan, result);
        }
        return result;
    }
};

BiRefNetJobControllerCore::BiRefNetJobControllerCore()
    : impl_(std::make_unique<Impl>())
{
}

BiRefNetJobControllerCore::~BiRefNetJobControllerCore() = default;

bool BiRefNetJobControllerCore::start(
    const BiRefNetJobRequestCore& request,
    std::string* errorOut)
{
    if (errorOut) errorOut->clear();
    std::error_code error;
    if (!fs::is_regular_file(request.scriptPath, error) || error) {
        if (errorOut) *errorOut = "birefnet.sh was not found.";
        return false;
    }
    error.clear();
    if (!fs::is_regular_file(request.mediaPath, error) || error) {
        if (errorOut) *errorOut =
            "BiRefNet source was not found.";
        return false;
    }
    if (request.model.empty() || request.revision.empty() ||
        request.modelCachePath.empty() ||
        request.runtimeCachePath.empty()) {
        if (errorOut) *errorOut =
            "BiRefNet model, revision, and cache paths are required.";
        return false;
    }
    if (impl_->process.snapshot().active()) {
        if (errorOut) *errorOut =
            "A BiRefNet job is already running.";
        return false;
    }
    impl_->process.wait();
    const BiRefNetJobPlanCore plan =
        buildBiRefNetJobPlanCore(request);
    if (request.restart) {
        const fs::path sourceParent =
            fs::absolute(request.mediaPath).parent_path();
        const fs::path output =
            fs::absolute(plan.outputDirectory);
        const fs::path jobRoot = fs::absolute(plan.jobRoot);
        const auto safelyInside = [&sourceParent](const fs::path& path) {
            const fs::path relative =
                path.lexically_relative(sourceParent);
            return !relative.empty() &&
                *relative.begin() != "..";
        };
        if (!safelyInside(output) || !safelyInside(jobRoot)) {
            if (errorOut) *errorOut =
                "BiRefNet restart paths escaped the source directory.";
            return false;
        }
        fs::remove_all(output, error);
        if (error) {
            if (errorOut) *errorOut =
                "Could not clear prior BiRefNet output.";
            return false;
        }
        error.clear();
        fs::remove_all(jobRoot, error);
        if (error) {
            if (errorOut) *errorOut =
                "Could not clear prior BiRefNet job.";
            return false;
        }
    }
    fs::create_directories(plan.outputDirectory, error);
    if (error) {
        if (errorOut) *errorOut =
            "Could not create BiRefNet output directory.";
        return false;
    }
    fs::create_directories(request.modelCachePath, error);
    if (error) {
        if (errorOut) *errorOut =
            "Could not create BiRefNet model cache.";
        return false;
    }
    fs::create_directories(request.runtimeCachePath, error);
    if (error) {
        if (errorOut) *errorOut =
            "Could not create BiRefNet runtime cache.";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->request = request;
        impl_->plan = plan;
        impl_->lastManifestState =
            ProcessJobSnapshotCore::State::Idle;
    }
    BiRefNetJobSnapshotCore prepared;
    prepared.state = ProcessJobSnapshotCore::State::Starting;
    prepared.clipId = request.clipId;
    prepared.status = "Preparing BiRefNet alpha generation.";
    prepared.jobRoot = plan.jobRoot;
    prepared.manifestPath = plan.manifestPath;
    prepared.logPath = plan.logPath;
    prepared.progressPath = plan.progressPath;
    prepared.livePreviewPath = plan.livePreviewPath;
    prepared.outputDirectory = plan.outputDirectory;
    if (!writeManifest(plan, prepared)) {
        if (errorOut) *errorOut =
            "Could not write BiRefNet job manifest.";
        return false;
    }
    return impl_->process.start(
        ProcessJobRequestCore{
            plan.command,
            plan.environment,
            fs::absolute(request.scriptPath)
                .parent_path().string(),
            plan.logPath,
            "Preparing BiRefNet alpha generation.",
            "BiRefNet alpha generation is running.",
            "BiRefNet alpha generation completed.",
            "BiRefNet stopped; completed frames are resumable.",
            "BiRefNet alpha generation failed."},
        errorOut);
}

void BiRefNetJobControllerCore::cancel()
{
    impl_->process.cancel();
}

BiRefNetJobSnapshotCore BiRefNetJobControllerCore::snapshot()
{
    return impl_->snapshot();
}

void BiRefNetJobControllerCore::wait()
{
    impl_->process.wait();
}

} // namespace jcut::jobs
