#pragma once

#include "core/image_buffer.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace jcut {

struct RuntimeControlSnapshot {
    nlohmann::json document;
    nlohmann::json renderStatus;
    nlohmann::json profile;
    core::ImageBuffer screenshot;
};

struct RuntimeControlProvider {
    std::function<RuntimeControlSnapshot()> snapshot;
    std::function<core::ImageBuffer()> screenshot;
    std::function<bool(std::int64_t frame, std::string* error)> setPlayhead;
};

class RuntimeControlServer {
public:
    RuntimeControlServer() = default;
    ~RuntimeControlServer();

    RuntimeControlServer(const RuntimeControlServer&) = delete;
    RuntimeControlServer& operator=(const RuntimeControlServer&) = delete;

    bool start(std::uint16_t port, RuntimeControlProvider provider, std::string* error);
    void stop();
    [[nodiscard]] bool running() const { return m_running; }
    [[nodiscard]] std::uint16_t port() const { return m_port; }

private:
    void run();
    void handleClient(int clientFd);

    RuntimeControlProvider m_provider;
    std::thread m_thread;
    int m_serverFd = -1;
    std::uint16_t m_port = 0;
    bool m_running = false;
};

std::uint16_t runtimeControlPortFromEnvironment(std::uint16_t fallback);

} // namespace jcut
