#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jcut::jobs {

struct ProcessJobRequestCore {
    std::vector<std::string> command;
    std::map<std::string, std::string> environmentOverrides;
    std::string workingDirectory;
    std::string logPath;
    std::string startingStatus = "Starting process.";
    std::string runningStatus = "Process is running.";
    std::string completedStatus = "Process completed.";
    std::string canceledStatus = "Process canceled.";
    std::string failedStatus = "Process failed.";
};

struct ProcessJobSnapshotCore {
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
    int processId = -1;
    int exitCode = -1;
    std::string status;
    std::string logPath;

    bool active() const noexcept
    {
        return state == State::Starting || state == State::Running ||
            state == State::Canceling;
    }
};

class ProcessJobControllerCore {
public:
    ProcessJobControllerCore();
    ~ProcessJobControllerCore();
    ProcessJobControllerCore(const ProcessJobControllerCore&) = delete;
    ProcessJobControllerCore& operator=(
        const ProcessJobControllerCore&) = delete;

    bool start(const ProcessJobRequestCore& request,
               std::string* errorOut = nullptr);
    bool writeStdin(std::string text,
                    std::string* errorOut = nullptr);
    void cancel();
    ProcessJobSnapshotCore snapshot() const;
    void wait();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace jcut::jobs
