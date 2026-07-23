#pragma once

#include "process_job_core.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace jcut::jobs {

struct BiRefNetJobRequestCore {
    int clipId = 0;
    std::string scriptPath;
    std::string mediaPath;
    std::string model = "ZhengPeng7/BiRefNet-matting";
    std::string revision =
        "57f9f68b43ba337c75762b14cf3075d659007268";
    std::string modelCachePath;
    std::string runtimeCachePath;
    std::string outputDirectory;
    std::string device = "cuda";
    bool fp16 = true;
    bool runDockerAsRoot = false;
    bool restart = false;
    double alphaTolerance = 0.0;
};

struct BiRefNetJobPlanCore {
    std::string jobRoot;
    std::string manifestPath;
    std::string logPath;
    std::string progressPath;
    std::string livePreviewPath;
    std::string outputDirectory;
    std::string containerName;
    std::vector<std::string> command;
    std::map<std::string, std::string> environment;
    nlohmann::json manifest;
};

struct BiRefNetJobSnapshotCore {
    ProcessJobSnapshotCore::State state =
        ProcessJobSnapshotCore::State::Idle;
    int clipId = 0;
    int processId = -1;
    int exitCode = -1;
    std::int64_t currentFrame = 0;
    std::int64_t totalFrames = 0;
    double percent = 0.0;
    std::string status;
    std::string jobRoot;
    std::string manifestPath;
    std::string logPath;
    std::string progressPath;
    std::string livePreviewPath;
    std::string outputDirectory;
    bool outputReady = false;

    bool active() const noexcept
    {
        return state == ProcessJobSnapshotCore::State::Starting ||
            state == ProcessJobSnapshotCore::State::Running ||
            state == ProcessJobSnapshotCore::State::Canceling;
    }
};

BiRefNetJobPlanCore buildBiRefNetJobPlanCore(
    const BiRefNetJobRequestCore& request);

class BiRefNetJobControllerCore {
public:
    BiRefNetJobControllerCore();
    ~BiRefNetJobControllerCore();
    BiRefNetJobControllerCore(const BiRefNetJobControllerCore&) = delete;
    BiRefNetJobControllerCore& operator=(
        const BiRefNetJobControllerCore&) = delete;

    bool start(const BiRefNetJobRequestCore& request,
               std::string* errorOut = nullptr);
    void cancel();
    BiRefNetJobSnapshotCore snapshot();
    void wait();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace jcut::jobs
