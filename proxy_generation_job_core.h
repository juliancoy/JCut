#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace jcut {

enum class ProxyGenerationFormat {
    ImageSequenceJpeg,
    H264Mp4,
    MjpegMov,
};

struct ProxyGenerationJobRequest {
    int clipId = 0;
    std::string sourcePath;
    std::string outputDirectory;
    ProxyGenerationFormat format = ProxyGenerationFormat::ImageSequenceJpeg;
    bool resume = false;
    bool overwrite = false;
};

struct ProxyGenerationJobSnapshot {
    enum class State {
        Idle,
        Starting,
        Running,
        Canceling,
        Completed,
        Canceled,
        Failed,
    };

    State state = State::Idle;
    int clipId = 0;
    std::int64_t framesCompleted = 0;
    std::int64_t totalFrames = 0;
    std::string status;
    std::string outputDirectory;
    std::string manifestPath;
    ProxyGenerationFormat format = ProxyGenerationFormat::ImageSequenceJpeg;

    bool active() const noexcept {
        return state == State::Starting || state == State::Running ||
            state == State::Canceling;
    }
};

std::string defaultProxyOutputPath(
    const std::string& sourcePath,
    ProxyGenerationFormat format);
bool removeProxyArtifact(const std::string& sourcePath,
                         const std::string& proxyPath,
                         std::string* errorOut = nullptr);

class ProxyGenerationJobController {
public:
    ProxyGenerationJobController();
    ~ProxyGenerationJobController();
    ProxyGenerationJobController(const ProxyGenerationJobController&) = delete;
    ProxyGenerationJobController& operator=(
        const ProxyGenerationJobController&) = delete;

    bool start(const ProxyGenerationJobRequest& request,
               std::string* errorOut = nullptr);
    void cancel();
    ProxyGenerationJobSnapshot snapshot() const;
    void wait();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace jcut
