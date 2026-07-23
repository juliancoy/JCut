#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jcut {

struct FaceProcessingJobRequest {
    std::string executablePath;
    std::string mediaPath;
    std::string transcriptPath;
    std::string clipId;
    std::string outputDirectory;
    std::string detectorSettingsPath;
    std::int64_t startFrame = 0;
    std::int64_t maxFrames = 0;
    int stride = 1;
    int detectorWorkers = 2;
    int detectorPipelineSlots = 2;
    double threshold = 0.5;
    bool primaryFaceOnly = false;
    bool smallFaceFallback = true;
    bool scrfdTiling = false;
    bool allowCpuUploadFallback = true;
};

struct FaceProcessingJobSnapshot {
    enum class State {
        Idle,
        Starting,
        Running,
        Canceling,
        Paused,
        Completed,
        Failed,
    };

    State state = State::Idle;
    int processId = -1;
    int exitCode = -1;
    std::string status;
    std::string outputDirectory;
    std::string manifestPath;
    std::string logPath;

    bool active() const noexcept {
        return state == State::Starting || state == State::Running ||
            state == State::Canceling;
    }
};

std::string faceProcessingSidecarDirectory(const std::string& mediaPath,
                                           const std::string& clipId);
std::vector<std::string> faceProcessingCommand(
    const FaceProcessingJobRequest& request);

class FaceProcessingJobController {
public:
    FaceProcessingJobController();
    ~FaceProcessingJobController();
    FaceProcessingJobController(const FaceProcessingJobController&) = delete;
    FaceProcessingJobController& operator=(const FaceProcessingJobController&) = delete;

    bool start(const FaceProcessingJobRequest& request,
               std::string* errorOut = nullptr);
    void cancel();
    FaceProcessingJobSnapshot snapshot() const;
    void wait();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace jcut
