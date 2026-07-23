#pragma once

#include "process_job_core.h"

#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace jcut::jobs {

struct TranscriptionJobRequestCore {
    int clipId = 0;
    std::string scriptPath;
    std::string mediaPath;
};

struct TranscriptionJobPlanCore {
    std::string jobRoot;
    std::string manifestPath;
    std::string logPath;
    std::string outputTranscriptPath;
    std::string workingDirectory;
    std::vector<std::string> command;
    nlohmann::json manifest;
};

struct TranscriptionJobSnapshotCore {
    ProcessJobSnapshotCore::State state =
        ProcessJobSnapshotCore::State::Idle;
    int clipId = 0;
    int processId = -1;
    int exitCode = -1;
    std::string status;
    std::string jobRoot;
    std::string manifestPath;
    std::string logPath;
    std::string outputTranscriptPath;
    bool outputReady = false;

    bool active() const noexcept
    {
        return state == ProcessJobSnapshotCore::State::Starting ||
            state == ProcessJobSnapshotCore::State::Running ||
            state == ProcessJobSnapshotCore::State::Canceling;
    }
};

TranscriptionJobPlanCore buildTranscriptionJobPlanCore(
    const TranscriptionJobRequestCore& request);
bool writeTranscriptionJobManifestCore(
    const TranscriptionJobPlanCore& plan,
    const TranscriptionJobSnapshotCore& snapshot,
    std::string* errorOut = nullptr);

class TranscriptionJobControllerCore {
public:
    TranscriptionJobControllerCore();
    ~TranscriptionJobControllerCore();
    TranscriptionJobControllerCore(
        const TranscriptionJobControllerCore&) = delete;
    TranscriptionJobControllerCore& operator=(
        const TranscriptionJobControllerCore&) = delete;

    bool start(const TranscriptionJobRequestCore& request,
               std::string* errorOut = nullptr);
    bool writeStdin(std::string text,
                    std::string* errorOut = nullptr);
    void cancel();
    TranscriptionJobSnapshotCore snapshot();
    void wait();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace jcut::jobs
