#include "runtime_control_server.h"

#include "editor_document_core_json.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "external/ncnn/src/stb_image_write.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string_view>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace jcut {
namespace {

struct HttpRequest {
    std::string method;
    std::string path;
    std::string body;
};

std::int64_t uptimeSeconds(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::steady_clock::now() - start)
        .count();
}

const std::chrono::steady_clock::time_point kProcessStart = std::chrono::steady_clock::now();

void closeFd(int& fd)
{
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
}

std::string reasonPhrase(int status)
{
    switch (status) {
    case 200:
        return "OK";
    case 400:
        return "Bad Request";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 500:
        return "Internal Server Error";
    case 503:
        return "Service Unavailable";
    default:
        return "OK";
    }
}

void writeResponse(int clientFd,
                   int status,
                   const std::string& contentType,
                   const std::string& body)
{
    std::ostringstream headers;
    headers << "HTTP/1.1 " << status << ' ' << reasonPhrase(status) << "\r\n"
            << "Content-Type: " << contentType << "\r\n"
            << "Content-Length: " << body.size() << "\r\n"
            << "Connection: close\r\n"
            << "Access-Control-Allow-Origin: *\r\n"
            << "\r\n";
    const std::string headerBytes = headers.str();
    send(clientFd, headerBytes.data(), headerBytes.size(), MSG_NOSIGNAL);
    if (!body.empty()) {
        send(clientFd, body.data(), body.size(), MSG_NOSIGNAL);
    }
}

void writeJson(int clientFd, int status, const nlohmann::json& body)
{
    writeResponse(clientFd, status, "application/json", body.dump(2));
}

void writeError(int clientFd, int status, const std::string& error)
{
    writeJson(clientFd, status, nlohmann::json{{"ok", false}, {"error", error}});
}

std::string stripQuery(std::string path)
{
    const std::size_t query = path.find('?');
    if (query != std::string::npos) {
        path.resize(query);
    }
    return path;
}

bool parseRequest(const std::string& bytes, HttpRequest* request)
{
    const std::size_t lineEnd = bytes.find("\r\n");
    if (lineEnd == std::string::npos) {
        return false;
    }
    std::istringstream line(bytes.substr(0, lineEnd));
    std::string url;
    std::string version;
    if (!(line >> request->method >> url >> version)) {
        return false;
    }
    request->path = stripQuery(url);

    const std::size_t headerEnd = bytes.find("\r\n\r\n");
    if (headerEnd != std::string::npos) {
        request->body = bytes.substr(headerEnd + 4);
    }
    return true;
}

std::string readRequestBytes(int clientFd)
{
    std::string bytes;
    bytes.reserve(4096);
    std::array<char, 4096> buffer{};
    for (;;) {
        const ssize_t n = recv(clientFd, buffer.data(), buffer.size(), 0);
        if (n <= 0) {
            break;
        }
        bytes.append(buffer.data(), static_cast<std::size_t>(n));
        const std::size_t headerEnd = bytes.find("\r\n\r\n");
        if (headerEnd != std::string::npos) {
            std::size_t contentLength = 0;
            const std::string headers = bytes.substr(0, headerEnd);
            std::istringstream stream(headers);
            std::string headerLine;
            while (std::getline(stream, headerLine)) {
                if (!headerLine.empty() && headerLine.back() == '\r') {
                    headerLine.pop_back();
                }
                const std::string key = "Content-Length:";
                if (headerLine.size() >= key.size() &&
                    std::equal(key.begin(), key.end(), headerLine.begin(),
                               [](char a, char b) {
                                   return std::tolower(a) == std::tolower(b);
                               })) {
                    const std::string value = headerLine.substr(key.size());
                    contentLength = static_cast<std::size_t>(std::strtoull(value.c_str(), nullptr, 10));
                }
            }
            if (bytes.size() >= headerEnd + 4 + contentLength) {
                break;
            }
        }
        if (bytes.size() > 1024 * 1024) {
            break;
        }
    }
    return bytes;
}

void pngWriteCallback(void* context, void* data, int size)
{
    auto* bytes = static_cast<std::string*>(context);
    bytes->append(static_cast<const char*>(data), static_cast<std::size_t>(size));
}

std::string encodePng(const core::ImageBuffer& image)
{
    if (image.empty()) {
        return {};
    }
    std::string bytes;
    stbi_write_png_to_func(pngWriteCallback,
                           &bytes,
                           image.size.width,
                           image.size.height,
                           4,
                           image.bytes.data(),
                           image.strideBytes);
    return bytes;
}

nlohmann::json playheadJson(const nlohmann::json& document)
{
    const nlohmann::json transport = document.value("transport", nlohmann::json::object());
    return nlohmann::json{
        {"ok", true},
        {"pid", static_cast<std::int64_t>(getpid())},
        {"current_frame", transport.value("currentFrame", 0)},
        {"playback_active", transport.value("playbackActive", false)},
        {"playback_speed", transport.value("playbackSpeed", 1.0)},
        {"preview_zoom", transport.value("previewZoom", 1.0)}
    };
}

std::int64_t frameFromBody(const std::string& body, bool* ok)
{
    *ok = false;
    try {
        const nlohmann::json root = nlohmann::json::parse(body.empty() ? "{}" : body);
        if (!root.contains("frame") || !root["frame"].is_number_integer()) {
            return -1;
        }
        *ok = true;
        return root["frame"].get<std::int64_t>();
    } catch (...) {
        return -1;
    }
}

} // namespace

RuntimeControlServer::~RuntimeControlServer()
{
    stop();
}

bool RuntimeControlServer::start(std::uint16_t port,
                                 RuntimeControlProvider provider,
                                 std::string* error)
{
    if (m_running) {
        return true;
    }
    m_provider = std::move(provider);
    m_serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (m_serverFd < 0) {
        if (error) {
            *error = std::strerror(errno);
        }
        return false;
    }

    int one = 1;
    setsockopt(m_serverFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (bind(m_serverFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (error) {
            *error = std::strerror(errno);
        }
        closeFd(m_serverFd);
        return false;
    }
    if (listen(m_serverFd, 16) != 0) {
        if (error) {
            *error = std::strerror(errno);
        }
        closeFd(m_serverFd);
        return false;
    }

    m_port = port;
    m_running = true;
    m_thread = std::thread(&RuntimeControlServer::run, this);
    return true;
}

void RuntimeControlServer::stop()
{
    if (!m_running) {
        return;
    }
    m_running = false;
    shutdown(m_serverFd, SHUT_RDWR);
    closeFd(m_serverFd);
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void RuntimeControlServer::run()
{
    while (m_running) {
        sockaddr_in clientAddr{};
        socklen_t clientLen = sizeof(clientAddr);
        int clientFd = accept(m_serverFd, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
        if (clientFd < 0) {
            if (m_running) {
                continue;
            }
            break;
        }
        handleClient(clientFd);
        close(clientFd);
    }
}

void RuntimeControlServer::handleClient(int clientFd)
{
    HttpRequest request;
    if (!parseRequest(readRequestBytes(clientFd), &request)) {
        writeError(clientFd, 400, "invalid request");
        return;
    }

    if (request.method == "OPTIONS") {
        writeResponse(clientFd, 200, "text/plain", "");
        return;
    }

    RuntimeControlSnapshot snapshot;
    const bool routeNeedsSnapshot = request.path != "/screenshot";
    if (routeNeedsSnapshot && m_provider.snapshot) {
        snapshot = m_provider.snapshot();
    }

    if (request.method == "GET" && request.path == "/health") {
        const nlohmann::json transport =
            snapshot.document.value("transport", nlohmann::json::object());
        writeJson(clientFd, 200, nlohmann::json{
            {"ok", true},
            {"pid", static_cast<std::int64_t>(getpid())},
            {"port", m_port},
            {"uptime_seconds", uptimeSeconds(kProcessStart)},
            {"current_frame", transport.value("currentFrame", 0)},
            {"playback_active", transport.value("playbackActive", false)},
            {"backend", "imgui"},
            {"api", "runtime_control"}
        });
        return;
    }

    if (request.method == "GET" && request.path == "/version") {
        writeJson(clientFd, 200, nlohmann::json{
            {"ok", true},
            {"pid", static_cast<std::int64_t>(getpid())},
            {"uptime_seconds", uptimeSeconds(kProcessStart)},
            {"backend", "imgui"},
            {"api", "runtime_control"}
        });
        return;
    }

    if (request.method == "GET" && request.path == "/playhead") {
        writeJson(clientFd, 200, playheadJson(snapshot.document));
        return;
    }

    if (request.method == "POST" && request.path == "/playhead") {
        bool ok = false;
        const std::int64_t frame = frameFromBody(request.body, &ok);
        if (!ok || frame < 0) {
            writeError(clientFd, 400, "invalid frame number");
            return;
        }
        std::string error;
        if (!m_provider.setPlayhead || !m_provider.setPlayhead(frame, &error)) {
            writeError(clientFd, 503, error.empty() ? "failed to set playhead" : error);
            return;
        }
        writeJson(clientFd, 200, nlohmann::json{{"ok", true}, {"frame", frame}});
        return;
    }

    if (request.method == "GET" && (request.path == "/state" || request.path == "/document")) {
        writeJson(clientFd, 200, nlohmann::json{{"ok", true}, {"document", snapshot.document}});
        return;
    }

    if (request.method == "GET" && request.path == "/ui") {
        writeJson(clientFd, 200, nlohmann::json{
            {"ok", true},
            {"backend", "imgui"},
            {"document", snapshot.document}
        });
        return;
    }

    if (request.method == "GET" && request.path == "/profile") {
        writeJson(clientFd, 200, nlohmann::json{
            {"ok", true},
            {"live", true},
            {"profile", snapshot.profile},
            {"fast_snapshot", playheadJson(snapshot.document)}
        });
        return;
    }

    if (request.method == "GET" &&
        (request.path == "/render/status" || request.path == "/pipeline")) {
        writeJson(clientFd, 200, snapshot.renderStatus);
        return;
    }

    if (request.method == "GET" && request.path == "/screenshot") {
        core::ImageBuffer screenshot;
        if (m_provider.screenshot) {
            screenshot = m_provider.screenshot();
        }
        const std::string png = encodePng(screenshot);
        if (png.empty()) {
            writeError(clientFd, 404, "no render pipeline preview frame is available");
            return;
        }
        writeResponse(clientFd, 200, "image/png", png);
        return;
    }

    writeError(clientFd, 404, "unknown endpoint");
}

std::uint16_t runtimeControlPortFromEnvironment(std::uint16_t fallback)
{
    const char* value = std::getenv("JCUT_CONTROL_PORT");
    if (!value || !*value) {
        value = std::getenv("EDITOR_CONTROL_PORT");
    }
    if (!value || !*value) {
        return fallback;
    }
    const unsigned long parsed = std::strtoul(value, nullptr, 10);
    if (parsed == 0 || parsed > 65535) {
        return fallback;
    }
    return static_cast<std::uint16_t>(parsed);
}

} // namespace jcut
