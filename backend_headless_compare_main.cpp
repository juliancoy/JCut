#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <vector>

namespace {

struct CommandResult {
    int exitCode = -1;
    double elapsedSec = 0.0;
    std::string output;
};

struct RenderRunResult {
    std::string backend;
    std::string outputPath;
    std::string logPath;
    int exitCode = -1;
    double elapsedSec = 0.0;
    std::string outputHash;
    std::string firstFrameHash;
    std::string lastFrameHash;
    std::string requestedBackend;
    std::string effectiveBackend;
    bool backendFallbackApplied = false;
    std::string backendFallbackReason;
};

std::string shellEscape(const std::string& input)
{
    std::string out;
    out.reserve(input.size() + 8);
    out.push_back('\'');
    for (char c : input) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out.push_back(c);
        }
    }
    out.push_back('\'');
    return out;
}

CommandResult runCommand(const std::string& command)
{
    CommandResult result;
    const auto start = std::chrono::steady_clock::now();

    const std::string wrapped = command + " 2>&1";
    std::array<char, 4096> buf{};
    FILE* pipe = popen(wrapped.c_str(), "r");
    if (!pipe) {
        result.output = "failed to execute command";
        result.exitCode = -1;
        return result;
    }

    while (fgets(buf.data(), static_cast<int>(buf.size()), pipe)) {
        result.output += buf.data();
    }

    const int status = pclose(pipe);
    if (WIFEXITED(status)) {
        result.exitCode = WEXITSTATUS(status);
    } else {
        result.exitCode = -1;
    }

    const auto end = std::chrono::steady_clock::now();
    result.elapsedSec = std::chrono::duration<double>(end - start).count();
    return result;
}

bool commandExists(const std::string& command)
{
    const std::string probe = "command -v " + command + " >/dev/null 2>&1";
    return runCommand(probe).exitCode == 0;
}

std::string trim(const std::string& s)
{
    const auto first = s.find_first_not_of(" \t\n\r");
    if (first == std::string::npos) return {};
    const auto last = s.find_last_not_of(" \t\n\r");
    return s.substr(first, last - first + 1);
}

std::string jsonEscape(const std::string& s)
{
    std::ostringstream os;
    for (unsigned char c : s) {
        switch (c) {
        case '\\': os << "\\\\"; break;
        case '"': os << "\\\""; break;
        case '\n': os << "\\n"; break;
        case '\r': os << "\\r"; break;
        case '\t': os << "\\t"; break;
        default:
            if (c < 0x20) {
                os << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c) << std::dec;
            } else {
                os << static_cast<char>(c);
            }
            break;
        }
    }
    return os.str();
}

std::string firstLines(const std::string& text, std::size_t maxLines)
{
    std::istringstream is(text);
    std::ostringstream os;
    std::string line;
    std::size_t i = 0;
    while (i < maxLines && std::getline(is, line)) {
        os << line << '\n';
        ++i;
    }
    return trim(os.str());
}

std::optional<CommandResult> probeVulkan()
{
    if (!commandExists("vulkaninfo")) return std::nullopt;
    return runCommand("vulkaninfo --summary");
}

std::optional<CommandResult> probeOpenGL()
{
    if (commandExists("glxinfo")) return runCommand("glxinfo -B");
    if (commandExists("eglinfo")) return runCommand("eglinfo -B");
    return std::nullopt;
}

std::string parseSha256(const std::string& output)
{
    std::istringstream is(output);
    std::string token;
    is >> token;
    return token;
}

std::string fileSha256(const std::string& path)
{
    CommandResult r = runCommand("sha256sum " + shellEscape(path));
    if (r.exitCode != 0) return {};
    return parseSha256(r.output);
}

std::string extractFieldValue(const std::string& text, const std::string& field)
{
    std::istringstream is(text);
    std::string line;
    const std::string prefix = field + ":";
    while (std::getline(is, line)) {
        if (line.rfind(prefix, 0) == 0) {
            return trim(line.substr(prefix.size()));
        }
    }
    return {};
}

void writeProbeArtifacts(const std::string& outDir,
                         const std::optional<CommandResult>& vk,
                         const std::optional<CommandResult>& gl)
{
    const bool vkAvailable = vk.has_value() && vk->exitCode == 0;
    const bool glAvailable = gl.has_value() && gl->exitCode == 0;

    std::ofstream summary(outDir + "/summary.txt", std::ios::trunc);
    summary << "mode: no-qt-backend-probe\n";
    summary << "vulkan_tool_present: " << (vk.has_value() ? "yes" : "no") << "\n";
    summary << "opengl_tool_present: " << (gl.has_value() ? "yes" : "no") << "\n";
    summary << "vulkan_available: " << (vkAvailable ? "yes" : "no") << "\n";
    summary << "opengl_available: " << (glAvailable ? "yes" : "no") << "\n";
    if (vk.has_value()) {
        summary << "vulkan_exit_code: " << vk->exitCode << "\n";
        summary << "vulkan_elapsed_sec: " << std::fixed << std::setprecision(3) << vk->elapsedSec << "\n";
    }
    if (gl.has_value()) {
        summary << "opengl_exit_code: " << gl->exitCode << "\n";
        summary << "opengl_elapsed_sec: " << std::fixed << std::setprecision(3) << gl->elapsedSec << "\n";
    }
    summary << "\n== vulkan probe snippet ==\n";
    summary << (vk.has_value() ? firstLines(vk->output, 40) : "vulkaninfo not found") << "\n";
    summary << "\n== opengl probe snippet ==\n";
    summary << (gl.has_value() ? firstLines(gl->output, 40) : "glxinfo/eglinfo not found") << "\n";

    std::ofstream json(outDir + "/summary.json", std::ios::trunc);
    json << "{\n";
    json << "  \"mode\": \"no-qt-backend-probe\",\n";
    json << "  \"vulkan\": {\n";
    json << "    \"tool_present\": " << (vk.has_value() ? "true" : "false") << ",\n";
    json << "    \"available\": " << (vkAvailable ? "true" : "false") << ",\n";
    json << "    \"exit_code\": " << (vk.has_value() ? std::to_string(vk->exitCode) : "null") << ",\n";
    json << "    \"elapsed_sec\": " << (vk.has_value() ? std::to_string(vk->elapsedSec) : "null") << ",\n";
    json << "    \"snippet\": \"" << jsonEscape(vk.has_value() ? firstLines(vk->output, 40) : "vulkaninfo not found") << "\"\n";
    json << "  },\n";
    json << "  \"opengl\": {\n";
    json << "    \"tool_present\": " << (gl.has_value() ? "true" : "false") << ",\n";
    json << "    \"available\": " << (glAvailable ? "true" : "false") << ",\n";
    json << "    \"exit_code\": " << (gl.has_value() ? std::to_string(gl->exitCode) : "null") << ",\n";
    json << "    \"elapsed_sec\": " << (gl.has_value() ? std::to_string(gl->elapsedSec) : "null") << ",\n";
    json << "    \"snippet\": \"" << jsonEscape(gl.has_value() ? firstLines(gl->output, 40) : "glxinfo/eglinfo not found") << "\"\n";
    json << "  }\n";
    json << "}\n";

    std::ofstream vkLog(outDir + "/vulkan_probe.log", std::ios::trunc);
    vkLog << (vk.has_value() ? vk->output : "vulkaninfo not found\n");
    std::ofstream glLog(outDir + "/opengl_probe.log", std::ios::trunc);
    glLog << (gl.has_value() ? gl->output : "glxinfo/eglinfo not found\n");
}

RenderRunResult runRenderCase(const std::string& outDir,
                              const std::string& jcutBin,
                              const std::string& statePath,
                              const std::string& clipId,
                              const std::string& speakerId,
                              const std::string& format,
                              const std::string& backend)
{
    RenderRunResult rr;
    rr.backend = backend;
    rr.outputPath = outDir + "/" + backend + "." + format;
    rr.logPath = outDir + "/" + backend + ".log";
    const std::string firstPng = outDir + "/" + backend + "_first.png";
    const std::string lastPng = outDir + "/" + backend + "_last.png";

    const std::string cmd =
        "env JCUT_RENDER_BACKEND=" + backend + " " +
        shellEscape(jcutBin) + " --speaker-export-harness" +
        " --state " + shellEscape(statePath) +
        " --output " + shellEscape(rr.outputPath) +
        " --format " + shellEscape(format) +
        " --clip-id " + shellEscape(clipId) +
        " --speaker-id " + shellEscape(speakerId) +
        " > " + shellEscape(rr.logPath);

    CommandResult run = runCommand(cmd);
    rr.exitCode = run.exitCode;
    rr.elapsedSec = run.elapsedSec;

    {
        std::ifstream in(rr.logPath);
        std::stringstream buffer;
        buffer << in.rdbuf();
        const std::string logText = buffer.str();
        rr.requestedBackend = extractFieldValue(logText, "  requestedBackend");
        rr.effectiveBackend = extractFieldValue(logText, "  effectiveBackend");
        rr.backendFallbackApplied =
            (extractFieldValue(logText, "  backendFallbackApplied") == "true");
        rr.backendFallbackReason = extractFieldValue(logText, "  backendFallbackReason");
    }

    if (rr.exitCode == 0 && std::filesystem::exists(rr.outputPath)) {
        rr.outputHash = fileSha256(rr.outputPath);
        if (commandExists("ffmpeg")) {
            runCommand("ffmpeg -y -i " + shellEscape(rr.outputPath) + " -frames:v 1 " + shellEscape(firstPng));
            runCommand("ffmpeg -y -sseof -0.05 -i " + shellEscape(rr.outputPath) + " -vframes 1 " + shellEscape(lastPng));
            if (std::filesystem::exists(firstPng)) rr.firstFrameHash = fileSha256(firstPng);
            if (std::filesystem::exists(lastPng)) rr.lastFrameHash = fileSha256(lastPng);
        }
    }

    return rr;
}

int runRenderCompare(const std::string& outDir,
                     const std::string& jcutBin,
                     const std::string& statePath,
                     const std::string& clipId,
                     const std::string& speakerId,
                     const std::string& format)
{
    const RenderRunResult gl = runRenderCase(outDir, jcutBin, statePath, clipId, speakerId, format, "opengl");
    const RenderRunResult vk = runRenderCase(outDir, jcutBin, statePath, clipId, speakerId, format, "vulkan");

    const bool renderSuccess = (gl.exitCode == 0) && (vk.exitCode == 0);
    const bool strictBackendMatch = (vk.effectiveBackend == "vulkan");
    const bool outputHashMatch = !gl.outputHash.empty() && gl.outputHash == vk.outputHash;
    const bool firstHashMatch = !gl.firstFrameHash.empty() && gl.firstFrameHash == vk.firstFrameHash;
    const bool lastHashMatch = !gl.lastFrameHash.empty() && gl.lastFrameHash == vk.lastFrameHash;

    std::ofstream summary(outDir + "/summary.txt", std::ios::trunc);
    summary << "mode: no-qt-render-compare-driver\n";
    summary << "jcut_bin: " << jcutBin << "\n";
    summary << "state_file: " << statePath << "\n";
    summary << "clip_id: " << clipId << "\n";
    summary << "speaker_id: " << speakerId << "\n";
    summary << "format: " << format << "\n\n";
    summary << "opengl_exit_code: " << gl.exitCode << "\n";
    summary << "vulkan_exit_code: " << vk.exitCode << "\n";
    summary << "opengl_effective_backend: " << gl.effectiveBackend << "\n";
    summary << "vulkan_effective_backend: " << vk.effectiveBackend << "\n";
    summary << "vulkan_strict_backend_match: " << (strictBackendMatch ? "yes" : "no") << "\n";
    summary << "opengl_elapsed_sec: " << std::fixed << std::setprecision(3) << gl.elapsedSec << "\n";
    summary << "vulkan_elapsed_sec: " << std::fixed << std::setprecision(3) << vk.elapsedSec << "\n";
    summary << "output_hash_match: " << (outputHashMatch ? "yes" : "no") << "\n";
    summary << "first_frame_hash_match: " << (firstHashMatch ? "yes" : "no") << "\n";
    summary << "last_frame_hash_match: " << (lastHashMatch ? "yes" : "no") << "\n";
    summary << "\n== hashes ==\n";
    summary << "opengl_output_sha256: " << gl.outputHash << "\n";
    summary << "vulkan_output_sha256: " << vk.outputHash << "\n";
    summary << "opengl_first_sha256: " << gl.firstFrameHash << "\n";
    summary << "vulkan_first_sha256: " << vk.firstFrameHash << "\n";
    summary << "opengl_last_sha256: " << gl.lastFrameHash << "\n";
    summary << "vulkan_last_sha256: " << vk.lastFrameHash << "\n";

    std::ofstream json(outDir + "/summary.json", std::ios::trunc);
    json << "{\n";
    json << "  \"mode\": \"no-qt-render-compare-driver\",\n";
    json << "  \"render_success\": " << (renderSuccess ? "true" : "false") << ",\n";
    json << "  \"vulkan_strict_backend_match\": " << (strictBackendMatch ? "true" : "false") << ",\n";
    json << "  \"output_hash_match\": " << (outputHashMatch ? "true" : "false") << ",\n";
    json << "  \"first_frame_hash_match\": " << (firstHashMatch ? "true" : "false") << ",\n";
    json << "  \"last_frame_hash_match\": " << (lastHashMatch ? "true" : "false") << ",\n";
    json << "  \"opengl\": {\n";
    json << "    \"exit_code\": " << gl.exitCode << ",\n";
    json << "    \"elapsed_sec\": " << gl.elapsedSec << ",\n";
    json << "    \"effective_backend\": \"" << gl.effectiveBackend << "\",\n";
    json << "    \"output_sha256\": \"" << gl.outputHash << "\"\n";
    json << "  },\n";
    json << "  \"vulkan\": {\n";
    json << "    \"exit_code\": " << vk.exitCode << ",\n";
    json << "    \"elapsed_sec\": " << vk.elapsedSec << ",\n";
    json << "    \"effective_backend\": \"" << vk.effectiveBackend << "\",\n";
    json << "    \"output_sha256\": \"" << vk.outputHash << "\"\n";
    json << "  }\n";
    json << "}\n";

    return (renderSuccess && strictBackendMatch && outputHashMatch) ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
    std::string mode = "probe";
    std::string outDir = "./testbench_assets/backend_headless_compare";
    std::string jcutBin = "./build/jcut";
    std::string statePath = "./testbench_state.json";
    std::string clipId = "clip_video_01";
    std::string speakerId = "spk1";
    std::string format = "mp4";

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--mode" && i + 1 < argc) {
            mode = argv[++i];
        } else if (arg == "--out-dir" && i + 1 < argc) {
            outDir = argv[++i];
        } else if (arg == "--jcut-bin" && i + 1 < argc) {
            jcutBin = argv[++i];
        } else if (arg == "--state" && i + 1 < argc) {
            statePath = argv[++i];
        } else if (arg == "--clip-id" && i + 1 < argc) {
            clipId = argv[++i];
        } else if (arg == "--speaker-id" && i + 1 < argc) {
            speakerId = argv[++i];
        } else if (arg == "--format" && i + 1 < argc) {
            format = argv[++i];
        } else if (arg == "--help") {
            std::cout
                << "Usage:\n"
                << "  jcut_headless_backend_compare [--mode probe|render_compare] [--out-dir DIR]\n"
                << "  probe mode options: none\n"
                << "  render_compare options: [--jcut-bin PATH] [--state PATH] [--clip-id ID] [--speaker-id ID] [--format FMT]\n";
            return 0;
        }
    }

    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    if (ec) {
        std::cerr << "failed to create output directory: " << outDir << " error=" << ec.message() << "\n";
        return 2;
    }

    int code = 0;
    if (mode == "render_compare") {
        code = runRenderCompare(outDir, jcutBin, statePath, clipId, speakerId, format);
    } else {
        const auto vk = probeVulkan();
        const auto gl = probeOpenGL();
        writeProbeArtifacts(outDir, vk, gl);
        const bool vkAvailable = vk.has_value() && vk->exitCode == 0;
        const bool glAvailable = gl.has_value() && gl->exitCode == 0;
        code = (vkAvailable && glAvailable) ? 0 : 1;
    }

    std::cout << "Wrote artifacts to: " << outDir << "\n";
    return code;
}
