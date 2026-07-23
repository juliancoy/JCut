#include "process_job_core.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace jcut::jobs {
namespace {

class ScopedSigpipeBlock {
public:
    ScopedSigpipeBlock()
    {
        sigemptyset(&set_);
        sigaddset(&set_, SIGPIPE);
        sigset_t pending;
        sigpending(&pending);
        wasPending_ = sigismember(&pending, SIGPIPE) == 1;
        blocked_ =
            pthread_sigmask(SIG_BLOCK, &set_, &oldMask_) == 0;
    }

    ~ScopedSigpipeBlock()
    {
        if (!blocked_) return;
        if (!wasPending_) {
            sigset_t pending;
            sigpending(&pending);
            if (sigismember(&pending, SIGPIPE) == 1) {
                const timespec noWait{0, 0};
                (void)sigtimedwait(&set_, nullptr, &noWait);
            }
        }
        (void)pthread_sigmask(SIG_SETMASK, &oldMask_, nullptr);
    }

private:
    sigset_t set_{};
    sigset_t oldMask_{};
    bool blocked_ = false;
    bool wasPending_ = false;
};

std::vector<std::string> processEnvironment(
    const std::map<std::string, std::string>& overrides)
{
    std::unordered_map<std::string, std::string> values;
    for (char** item = environ; item && *item; ++item) {
        const std::string entry(*item);
        const std::size_t separator = entry.find('=');
        if (separator != std::string::npos) {
            values[entry.substr(0, separator)] =
                entry.substr(separator + 1);
        }
    }
    for (const auto& [name, value] : overrides) {
        if (value.empty()) {
            values.erase(name);
        } else {
            values[name] = value;
        }
    }
    std::vector<std::string> result;
    result.reserve(values.size());
    for (const auto& [name, value] : values) {
        result.push_back(name + "=" + value);
    }
    return result;
}

} // namespace

struct ProcessJobControllerCore::Impl {
    mutable std::mutex mutex;
    std::thread worker;
    std::atomic<bool> cancelRequested{false};
    ProcessJobRequestCore request;
    ProcessJobSnapshotCore current;
    int stdinWriteFd = -1;

    void publish(ProcessJobSnapshotCore::State state,
                 std::string status,
                 int processId = -1,
                 int exitCode = -1)
    {
        std::lock_guard<std::mutex> lock(mutex);
        current.state = state;
        current.status = std::move(status);
        if (processId >= 0) current.processId = processId;
        if (exitCode >= 0) current.exitCode = exitCode;
    }

    void closeInput()
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (stdinWriteFd >= 0) {
            ::close(stdinWriteFd);
            stdinWriteFd = -1;
        }
    }

    void run()
    {
        int inputPipe[2]{-1, -1};
        if (::pipe2(inputPipe, O_CLOEXEC) != 0) {
            publish(
                ProcessJobSnapshotCore::State::Failed,
                request.failedStatus + " Could not create stdin pipe.");
            return;
        }
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(
            &actions, inputPipe[0], STDIN_FILENO);
        posix_spawn_file_actions_addclose(&actions, inputPipe[1]);
        posix_spawn_file_actions_addopen(
            &actions,
            STDOUT_FILENO,
            request.logPath.c_str(),
            O_WRONLY | O_CREAT | O_TRUNC,
            0644);
        posix_spawn_file_actions_adddup2(
            &actions, STDOUT_FILENO, STDERR_FILENO);
        if (!request.workingDirectory.empty()) {
#if defined(__GLIBC__)
            posix_spawn_file_actions_addchdir_np(
                &actions, request.workingDirectory.c_str());
#endif
        }

        posix_spawnattr_t attributes;
        posix_spawnattr_init(&attributes);
        posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
        posix_spawnattr_setpgroup(&attributes, 0);

        std::vector<std::string> command = request.command;
        std::vector<char*> arguments;
        arguments.reserve(command.size() + 1);
        for (std::string& argument : command) {
            arguments.push_back(argument.data());
        }
        arguments.push_back(nullptr);
        std::vector<std::string> environment =
            processEnvironment(request.environmentOverrides);
        std::vector<char*> environmentPointers;
        environmentPointers.reserve(environment.size() + 1);
        for (std::string& entry : environment) {
            environmentPointers.push_back(entry.data());
        }
        environmentPointers.push_back(nullptr);

        pid_t child = -1;
        const int spawnResult = posix_spawn(
            &child,
            command.front().c_str(),
            &actions,
            &attributes,
            arguments.data(),
            environmentPointers.data());
        posix_spawnattr_destroy(&attributes);
        posix_spawn_file_actions_destroy(&actions);
        ::close(inputPipe[0]);
        if (spawnResult != 0) {
            ::close(inputPipe[1]);
            publish(
                ProcessJobSnapshotCore::State::Failed,
                request.failedStatus + " Spawn error " +
                    std::to_string(spawnResult) + ".");
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            stdinWriteFd = inputPipe[1];
        }
        publish(
            ProcessJobSnapshotCore::State::Running,
            request.runningStatus,
            static_cast<int>(child));

        int status = 0;
        bool termSent = false;
        auto cancelStarted = std::chrono::steady_clock::time_point{};
        for (;;) {
            const pid_t waited = ::waitpid(child, &status, WNOHANG);
            if (waited == child) break;
            if (waited < 0 && errno != EINTR) {
                closeInput();
                publish(
                    ProcessJobSnapshotCore::State::Failed,
                    request.failedStatus + " Could not wait for process.");
                return;
            }
            if (cancelRequested.load()) {
                if (!termSent) {
                    publish(
                        ProcessJobSnapshotCore::State::Canceling,
                        "Stopping process safely.");
                    ::kill(-child, SIGTERM);
                    termSent = true;
                    cancelStarted = std::chrono::steady_clock::now();
                } else if (
                    std::chrono::steady_clock::now() - cancelStarted >
                    std::chrono::seconds(5)) {
                    ::kill(-child, SIGKILL);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        closeInput();
        const int exitCode = WIFEXITED(status)
            ? WEXITSTATUS(status)
            : (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1);
        if (cancelRequested.load()) {
            publish(
                ProcessJobSnapshotCore::State::Canceled,
                request.canceledStatus,
                static_cast<int>(child),
                std::max(0, exitCode));
        } else if (WIFEXITED(status) && exitCode == 0) {
            publish(
                ProcessJobSnapshotCore::State::Completed,
                request.completedStatus,
                static_cast<int>(child),
                0);
        } else {
            publish(
                ProcessJobSnapshotCore::State::Failed,
                request.failedStatus + " Exit code " +
                    std::to_string(exitCode) + ".",
                static_cast<int>(child),
                std::max(0, exitCode));
        }
    }
};

ProcessJobControllerCore::ProcessJobControllerCore()
    : impl_(std::make_unique<Impl>())
{
}

ProcessJobControllerCore::~ProcessJobControllerCore()
{
    cancel();
    wait();
}

bool ProcessJobControllerCore::start(
    const ProcessJobRequestCore& request,
    std::string* errorOut)
{
    if (errorOut) errorOut->clear();
    if (request.command.empty() || request.command.front().empty()) {
        if (errorOut) *errorOut = "Process command is empty.";
        return false;
    }
    if (request.logPath.empty()) {
        if (errorOut) *errorOut = "Process log path is empty.";
        return false;
    }
    {
        const ProcessJobSnapshotCore snapshot = this->snapshot();
        if (snapshot.active()) {
            if (errorOut) *errorOut = "A process job is already running.";
            return false;
        }
    }
    wait();
    std::error_code directoryError;
    const std::filesystem::path logPath(request.logPath);
    if (!logPath.parent_path().empty()) {
        std::filesystem::create_directories(
            logPath.parent_path(), directoryError);
    }
    if (directoryError) {
        if (errorOut) {
            *errorOut = "Could not create process log directory.";
        }
        return false;
    }
    impl_->request = request;
    impl_->cancelRequested.store(false);
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->current = {};
        impl_->current.state = ProcessJobSnapshotCore::State::Starting;
        impl_->current.status = request.startingStatus;
        impl_->current.logPath = request.logPath;
    }
    impl_->worker = std::thread([implementation = impl_.get()] {
        implementation->run();
    });
    return true;
}

bool ProcessJobControllerCore::writeStdin(
    std::string text,
    std::string* errorOut)
{
    if (errorOut) errorOut->clear();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->stdinWriteFd < 0 ||
        impl_->current.state != ProcessJobSnapshotCore::State::Running) {
        if (errorOut) *errorOut = "Process stdin is unavailable.";
        return false;
    }
    if (!text.ends_with('\n')) text.push_back('\n');
    ScopedSigpipeBlock sigpipeBlock;
    std::size_t written = 0;
    while (written < text.size()) {
        const ssize_t count = ::write(
            impl_->stdinWriteFd,
            text.data() + written,
            text.size() - written);
        if (count < 0) {
            if (errno == EINTR) continue;
            if (errorOut) *errorOut = "Could not write process stdin.";
            return false;
        }
        written += static_cast<std::size_t>(count);
    }
    return true;
}

void ProcessJobControllerCore::cancel()
{
    if (snapshot().active()) {
        impl_->cancelRequested.store(true);
    }
}

ProcessJobSnapshotCore ProcessJobControllerCore::snapshot() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->current;
}

void ProcessJobControllerCore::wait()
{
    if (impl_->worker.joinable()) impl_->worker.join();
}

} // namespace jcut::jobs
