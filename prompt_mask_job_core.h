#pragma once

#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace jcut::masks {

enum class PromptMaskRestartPolicy {
    Resume,
    Restart,
};

struct PromptMaskJobRequest {
    std::string scriptPath;
    std::string mediaPath;
    std::string prompt;
    std::string modelCachePath;
    std::string runtimeCachePath;
    std::string currentMaskDirectory;
    int scaleWidth = 0;
    int prescaleWidth = 0;
    double extractFps = 0.0;
    std::string intermediateFramesFormat = "jpg";
    bool compileModel = false;
    bool videoMode = false;
    bool writeBinaryMasks = true;
    bool unionWithCurrentMask = false;
    bool writeMaskPreviewFrames = false;
    bool exportCentersJson = false;
    bool runDockerAsRoot = false;
    PromptMaskRestartPolicy restartPolicy = PromptMaskRestartPolicy::Resume;
};

struct PromptMaskJobPlan {
    std::string jobRoot;
    std::string manifestPath;
    std::string centersPath;
    std::string binaryMasksPath;
    std::string combinedMasksPath;
    std::string selectedMaskPath;
    std::string defaultOutputPath;
    std::string logPath;
    std::vector<std::string> command;
    nlohmann::json manifest;
};

struct PromptMaskJobSnapshot {
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
    std::string jobRoot;
    std::string manifestPath;
    std::string selectedMaskPath;
    std::string selectedMaskId;
    std::string selectedMaskName;
    std::string logPath;

    bool active() const noexcept
    {
        return state == State::Starting || state == State::Running ||
            state == State::Canceling;
    }
};

std::string sanitizedPromptMaskJobComponent(std::string value);
PromptMaskJobPlan buildPromptMaskJobPlan(
    const PromptMaskJobRequest& request);
bool writePromptMaskJobManifest(
    const PromptMaskJobPlan& plan,
    std::string* errorOut = nullptr);

class PromptMaskJobController {
public:
    PromptMaskJobController();
    ~PromptMaskJobController();
    PromptMaskJobController(const PromptMaskJobController&) = delete;
    PromptMaskJobController& operator=(const PromptMaskJobController&) = delete;

    bool start(const PromptMaskJobRequest& request,
               std::string* errorOut = nullptr);
    void cancel();
    PromptMaskJobSnapshot snapshot() const;
    void wait();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace jcut::masks
