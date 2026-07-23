#include "transcription_job_core.h"

#include "prompt_mask_job_core.h"

#include <filesystem>
#include <fstream>
#include <mutex>

namespace jcut::jobs {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

const char* stateName(ProcessJobSnapshotCore::State state)
{
    using State = ProcessJobSnapshotCore::State;
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

bool writeJsonAtomically(const fs::path& path,
                         const json& value,
                         std::string* errorOut)
{
    if (errorOut) errorOut->clear();
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    if (error) {
        if (errorOut) *errorOut =
            "Could not create transcription job directory.";
        return false;
    }
    const fs::path temporary = path.string() + ".tmp";
    {
        std::ofstream output(
            temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            if (errorOut) *errorOut =
                "Could not open transcription manifest.";
            return false;
        }
        output << value.dump(2) << '\n';
        if (!output.good()) {
            if (errorOut) *errorOut =
                "Could not write transcription manifest.";
            return false;
        }
    }
    fs::rename(temporary, path, error);
    if (error) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        if (errorOut) *errorOut =
            "Could not commit transcription manifest.";
        return false;
    }
    return true;
}

} // namespace

TranscriptionJobPlanCore buildTranscriptionJobPlanCore(
    const TranscriptionJobRequestCore& request)
{
    TranscriptionJobPlanCore plan;
    const fs::path script = fs::absolute(request.scriptPath);
    const fs::path media = fs::absolute(request.mediaPath);
    const std::string stem =
        jcut::masks::sanitizedPromptMaskJobComponent(
            media.stem().string());
    plan.jobRoot = (
        media.parent_path() / ".jcut_jobs" /
        ("transcription_" + stem)).string();
    plan.manifestPath =
        (fs::path(plan.jobRoot) / "manifest.json").string();
    plan.logPath =
        (fs::path(plan.jobRoot) / "job.log").string();
    plan.outputTranscriptPath =
        (media.parent_path() / (media.stem().string() + ".json")).string();
    plan.workingDirectory = script.parent_path().string();
    plan.command = {
        "/bin/bash", script.string(), media.string()};
    plan.manifest = {
        {"schema", "jcut_processing_job_v1"},
        {"operation", "transcription"},
        {"status", "prepared"},
        {"input", {
            {"path", media.string()},
            {"clip_id", request.clipId}}},
        {"artifacts", {
            {"job_root", plan.jobRoot},
            {"manifest", plan.manifestPath},
            {"log", plan.logPath},
            {"transcript", plan.outputTranscriptPath}}},
        {"command", plan.command},
    };
    return plan;
}

bool writeTranscriptionJobManifestCore(
    const TranscriptionJobPlanCore& plan,
    const TranscriptionJobSnapshotCore& snapshot,
    std::string* errorOut)
{
    json manifest = plan.manifest;
    manifest["status"] = stateName(snapshot.state);
    manifest["process_id"] = snapshot.processId;
    manifest["exit_code"] = snapshot.exitCode;
    manifest["message"] = snapshot.status;
    manifest["artifacts"]["transcript_ready"] =
        snapshot.outputReady;
    return writeJsonAtomically(
        plan.manifestPath, manifest, errorOut);
}

struct TranscriptionJobControllerCore::Impl {
    std::mutex mutex;
    ProcessJobControllerCore process;
    TranscriptionJobRequestCore request;
    TranscriptionJobPlanCore plan;
    ProcessJobSnapshotCore::State lastManifestState =
        ProcessJobSnapshotCore::State::Idle;

    TranscriptionJobSnapshotCore snapshot()
    {
        const ProcessJobSnapshotCore processSnapshot =
            process.snapshot();
        TranscriptionJobSnapshotCore result;
        {
            std::lock_guard<std::mutex> lock(mutex);
            result.clipId = request.clipId;
            result.jobRoot = plan.jobRoot;
            result.manifestPath = plan.manifestPath;
            result.logPath = plan.logPath;
            result.outputTranscriptPath =
                plan.outputTranscriptPath;
        }
        result.state = processSnapshot.state;
        result.processId = processSnapshot.processId;
        result.exitCode = processSnapshot.exitCode;
        result.status = processSnapshot.status;
        std::error_code error;
        result.outputReady =
            fs::is_regular_file(result.outputTranscriptPath, error) &&
            !error &&
            fs::file_size(result.outputTranscriptPath, error) > 0 &&
            !error;
        if (result.state != lastManifestState) {
            lastManifestState = result.state;
            (void)writeTranscriptionJobManifestCore(
                plan, result, nullptr);
        }
        return result;
    }
};

TranscriptionJobControllerCore::TranscriptionJobControllerCore()
    : impl_(std::make_unique<Impl>())
{
}

TranscriptionJobControllerCore::~TranscriptionJobControllerCore() =
    default;

bool TranscriptionJobControllerCore::start(
    const TranscriptionJobRequestCore& request,
    std::string* errorOut)
{
    if (errorOut) errorOut->clear();
    std::error_code error;
    if (!fs::is_regular_file(request.scriptPath, error) || error) {
        if (errorOut) *errorOut = "whisperx.sh was not found.";
        return false;
    }
    error.clear();
    if (!fs::is_regular_file(request.mediaPath, error) || error) {
        if (errorOut) *errorOut =
            "Transcription source was not found.";
        return false;
    }
    if (impl_->process.snapshot().active()) {
        if (errorOut) *errorOut =
            "A transcription job is already running.";
        return false;
    }
    impl_->process.wait();
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->request = request;
        impl_->plan = buildTranscriptionJobPlanCore(request);
        impl_->lastManifestState =
            ProcessJobSnapshotCore::State::Idle;
    }
    TranscriptionJobSnapshotCore prepared;
    prepared.state = ProcessJobSnapshotCore::State::Starting;
    prepared.clipId = request.clipId;
    prepared.status = "Preparing WhisperX transcription.";
    prepared.jobRoot = impl_->plan.jobRoot;
    prepared.manifestPath = impl_->plan.manifestPath;
    prepared.logPath = impl_->plan.logPath;
    prepared.outputTranscriptPath =
        impl_->plan.outputTranscriptPath;
    if (!writeTranscriptionJobManifestCore(
            impl_->plan, prepared, errorOut)) {
        return false;
    }
    return impl_->process.start(
        ProcessJobRequestCore{
            impl_->plan.command,
            {},
            impl_->plan.workingDirectory,
            impl_->plan.logPath,
            "Preparing WhisperX transcription.",
            "WhisperX transcription is running.",
            "WhisperX transcription completed.",
            "WhisperX transcription canceled.",
            "WhisperX transcription failed."},
        errorOut);
}

bool TranscriptionJobControllerCore::writeStdin(
    std::string text,
    std::string* errorOut)
{
    return impl_->process.writeStdin(
        std::move(text), errorOut);
}

void TranscriptionJobControllerCore::cancel()
{
    impl_->process.cancel();
}

TranscriptionJobSnapshotCore
TranscriptionJobControllerCore::snapshot()
{
    return impl_->snapshot();
}

void TranscriptionJobControllerCore::wait()
{
    impl_->process.wait();
}

} // namespace jcut::jobs
