#include "ai_credential_store_core.h"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace jcut::ai {
namespace {

namespace fs = std::filesystem;

std::string trim(std::string value)
{
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

fs::path executableInPath(const std::string& name)
{
    const char* pathValue = std::getenv("PATH");
    if (!pathValue) return {};
    std::string_view paths(pathValue);
    std::size_t offset = 0;
    while (offset <= paths.size()) {
        const std::size_t separator = paths.find(':', offset);
        const std::string_view part = paths.substr(
            offset,
            separator == std::string_view::npos
                ? paths.size() - offset : separator - offset);
        const fs::path candidate =
            fs::path(part.empty() ? "." : std::string(part)) / name;
        if (::access(candidate.c_str(), X_OK) == 0) return candidate;
        if (separator == std::string_view::npos) break;
        offset = separator + 1;
    }
    return {};
}

struct ProcessResult {
    int exitCode = -1;
    std::string output;
    std::string error;
};

ProcessResult runProcess(const fs::path& executable,
                         const std::vector<std::string>& arguments,
                         const std::string& input)
{
    ProcessResult result;
    int inputPipe[2]{-1, -1};
    int outputPipe[2]{-1, -1};
    int errorPipe[2]{-1, -1};
    if (::pipe(inputPipe) != 0 || ::pipe(outputPipe) != 0 ||
        ::pipe(errorPipe) != 0) {
        result.error = "Could not create credential-store pipes";
        for (int descriptor :
             {inputPipe[0], inputPipe[1], outputPipe[0], outputPipe[1],
              errorPipe[0], errorPipe[1]}) {
            if (descriptor >= 0) ::close(descriptor);
        }
        return result;
    }
    const pid_t child = ::fork();
    if (child < 0) {
        result.error = "Could not start credential-store process";
        for (int descriptor :
             {inputPipe[0], inputPipe[1], outputPipe[0], outputPipe[1],
              errorPipe[0], errorPipe[1]}) {
            ::close(descriptor);
        }
        return result;
    }
    if (child == 0) {
        ::dup2(inputPipe[0], STDIN_FILENO);
        ::dup2(outputPipe[1], STDOUT_FILENO);
        ::dup2(errorPipe[1], STDERR_FILENO);
        for (int descriptor :
             {inputPipe[0], inputPipe[1], outputPipe[0], outputPipe[1],
              errorPipe[0], errorPipe[1]}) {
            ::close(descriptor);
        }
        std::vector<std::string> owned;
        owned.reserve(arguments.size() + 1);
        owned.push_back(executable.string());
        owned.insert(owned.end(), arguments.begin(), arguments.end());
        std::vector<char*> argv;
        argv.reserve(owned.size() + 1);
        for (std::string& value : owned) argv.push_back(value.data());
        argv.push_back(nullptr);
        ::execve(executable.c_str(), argv.data(), environ);
        _exit(127);
    }
    ::close(inputPipe[0]);
    ::close(outputPipe[1]);
    ::close(errorPipe[1]);
    std::size_t written = 0;
    while (written < input.size()) {
        const ssize_t count = ::write(
            inputPipe[1], input.data() + written, input.size() - written);
        if (count > 0) written += static_cast<std::size_t>(count);
        else if (count < 0 && errno == EINTR) continue;
        else break;
    }
    ::close(inputPipe[1]);
    (void)::fcntl(outputPipe[0], F_SETFL,
                  ::fcntl(outputPipe[0], F_GETFL) | O_NONBLOCK);
    (void)::fcntl(errorPipe[0], F_SETFL,
                  ::fcntl(errorPipe[0], F_GETFL) | O_NONBLOCK);
    const auto drain = [](int descriptor, std::string* bytes) {
        char buffer[4096];
        for (;;) {
            const ssize_t count = ::read(descriptor, buffer, sizeof(buffer));
            if (count > 0) {
                bytes->append(buffer, static_cast<std::size_t>(count));
            } else if (count < 0 && errno == EINTR) {
                continue;
            } else {
                break;
            }
        }
    };
    int status = 0;
    bool finished = false;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!finished && std::chrono::steady_clock::now() < deadline) {
        pollfd descriptors[2]{
            {outputPipe[0], POLLIN, 0},
            {errorPipe[0], POLLIN, 0},
        };
        (void)::poll(descriptors, 2, 50);
        drain(outputPipe[0], &result.output);
        drain(errorPipe[0], &result.error);
        const pid_t waited = ::waitpid(child, &status, WNOHANG);
        finished = waited == child;
        if (waited < 0 && errno != EINTR) break;
    }
    if (!finished) {
        (void)::kill(child, SIGKILL);
        while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
        result.error = "Credential-store process timed out";
    }
    drain(outputPipe[0], &result.output);
    drain(errorPipe[0], &result.error);
    ::close(outputPipe[0]);
    ::close(errorPipe[0]);
    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
}

ProcessResult secretTool(const fs::path& executable,
                         const CredentialStoreConfigCore& config,
                         const std::string& operation,
                         const std::string& field,
                         const std::string& value = {})
{
    std::vector<std::string> arguments{operation};
    if (operation == "store") {
        arguments.push_back(
            "--label=" + config.serviceName + " " + field);
    }
    arguments.insert(
        arguments.end(),
        {"service", config.serviceName, "field", field});
    return runProcess(
        executable, arguments,
        operation == "store" ? value : std::string{});
}

fs::path configDirectory(const CredentialStoreConfigCore& config)
{
    if (!config.configDirectoryOverride.empty()) {
        return fs::path(config.configDirectoryOverride);
    }
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        return fs::path(xdg) / config.applicationName;
    }
    if (const char* userHome = std::getenv("HOME"); userHome && *userHome) {
        return fs::path(userHome) / ".config" / config.applicationName;
    }
    return {};
}

std::string readFile(const fs::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return {};
    return trim(std::string(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()));
}

bool writePrivateFile(const fs::path& path,
                      const std::string& value,
                      std::string* error)
{
    const int descriptor = ::open(
        path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        if (error) *error = "Could not open " + path.string();
        return false;
    }
    const std::string bytes = value + "\n";
    std::size_t written = 0;
    while (written < bytes.size()) {
        const ssize_t count = ::write(
            descriptor, bytes.data() + written, bytes.size() - written);
        if (count > 0) written += static_cast<std::size_t>(count);
        else if (count < 0 && errno == EINTR) continue;
        else {
            ::close(descriptor);
            if (error) *error = "Could not write " + path.string();
            return false;
        }
    }
    (void)::fsync(descriptor);
    ::close(descriptor);
    return true;
}

} // namespace

CredentialStoreResultCore loadStoredCredentialsCore(
    const CredentialStoreConfigCore& config)
{
    CredentialStoreResultCore result;
    const fs::path tool = config.preferSystemStore
        ? executableInPath("secret-tool") : fs::path{};
    if (!tool.empty()) {
        const ProcessResult token =
            secretTool(tool, config, "lookup", "token");
        if (token.exitCode == 0) {
            result.ok = true;
            result.usedSystemStore = true;
            result.credentials.accessToken = trim(token.output);
            result.credentials.refreshToken = trim(
                secretTool(tool, config, "lookup", "refresh_token").output);
            result.credentials.userId = trim(
                secretTool(tool, config, "lookup", "user_id").output);
            if (!result.credentials.accessToken.empty()) return result;
        }
    }
    const fs::path directory = configDirectory(config);
    if (directory.empty()) {
        result.error = "No user configuration directory is available";
        return result;
    }
    result.ok = true;
    result.credentials.accessToken =
        readFile(directory / "auth_token.txt");
    result.credentials.refreshToken =
        readFile(directory / "auth_refresh_token.txt");
    result.credentials.userId =
        readFile(directory / "auth_user_id.txt");
    return result;
}

CredentialStoreResultCore storeCredentialsCore(
    const StoredCredentialsCore& credentials,
    const CredentialStoreConfigCore& config)
{
    CredentialStoreResultCore result;
    if (trim(credentials.accessToken).empty()) {
        result.error = "Cannot store empty access credentials";
        return result;
    }
    const fs::path tool = config.preferSystemStore
        ? executableInPath("secret-tool") : fs::path{};
    if (!tool.empty()) {
        const ProcessResult token = secretTool(
            tool, config, "store", "token", credentials.accessToken);
        const ProcessResult refresh = secretTool(
            tool, config, "store", "refresh_token", credentials.refreshToken);
        const ProcessResult user = secretTool(
            tool, config, "store", "user_id", credentials.userId);
        if (token.exitCode == 0 && refresh.exitCode == 0 &&
            user.exitCode == 0) {
            result.ok = true;
            result.usedSystemStore = true;
            result.credentials = credentials;
            return result;
        }
    }
    const fs::path directory = configDirectory(config);
    std::error_code directoryError;
    if (directory.empty() ||
        (!fs::create_directories(directory, directoryError) &&
         directoryError)) {
        result.error = "Could not create the credential directory";
        return result;
    }
    (void)::chmod(directory.c_str(), 0700);
    if (!writePrivateFile(
            directory / "auth_token.txt",
            trim(credentials.accessToken), &result.error) ||
        !writePrivateFile(
            directory / "auth_refresh_token.txt",
            trim(credentials.refreshToken), &result.error) ||
        !writePrivateFile(
            directory / "auth_user_id.txt",
            trim(credentials.userId), &result.error)) {
        return result;
    }
    result.ok = true;
    result.credentials = credentials;
    return result;
}

CredentialStoreResultCore clearStoredCredentialsCore(
    const CredentialStoreConfigCore& config)
{
    CredentialStoreResultCore result;
    const fs::path tool = config.preferSystemStore
        ? executableInPath("secret-tool") : fs::path{};
    if (!tool.empty()) {
        for (const char* field : {"token", "refresh_token", "user_id"}) {
            (void)secretTool(tool, config, "clear", field);
        }
        result.usedSystemStore = true;
    }
    const fs::path directory = configDirectory(config);
    std::error_code ignored;
    if (!directory.empty()) {
        fs::remove(directory / "auth_token.txt", ignored);
        fs::remove(directory / "auth_refresh_token.txt", ignored);
        fs::remove(directory / "auth_user_id.txt", ignored);
    }
    result.ok = true;
    return result;
}

} // namespace jcut::ai
