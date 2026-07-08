#include "audio_engine.h"
#include "editor_runtime.h"
#include "editor_document_core_json.h"
#include "editor_document_render_bridge.h"
#include "editor_shared_render_sync.h"
#include "imgui_project_io.h"
#include "render_contract_json.h"
#include "render_runtime.h"
#include "runtime_control_server.h"
#include "standalone_export_renderer.h"
#include "standalone_preview_renderer.h"
#include "vulkan_detector_frame_handoff.h"

#include "external/imgui/imgui.h"
#include "external/imgui/backends/imgui_impl_vulkan.h"

#define VK_USE_PLATFORM_XLIB_KHR
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_xlib.h>

#ifdef None
#undef None
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <mutex>
#include <limits>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

enum class TimelineDragMode {
    None,
    Seek,
    MoveClip,
    TrimClipStart,
    TrimClipEnd,
};

constexpr float kTimelinePixelsPerFrame = 0.35f;
constexpr float kTimelineRowHeight = 34.0f;
constexpr float kTimelineLabelWidth = 90.0f;
constexpr float kTimelineClipHeight = 24.0f;
constexpr float kTimelineTrackPadding = 12.0f;
constexpr float kTimelineTopPadding = 10.0f;
constexpr float kTimelineHandleWidth = 8.0f;
constexpr float kShellGap = 10.0f;
constexpr float kStatusBarHeight = 28.0f;
constexpr float kMediaPanelWidth = 320.0f;
constexpr float kInspectorPanelWidth = 340.0f;
constexpr float kTimelinePanelHeight = 280.0f;
constexpr float kDefaultUiFontSize = 16.0f;
constexpr float kMinUiFontSize = 11.0f;
constexpr float kMaxUiFontSize = 28.0f;

struct ShellState {
    jcut::EditorRuntime runtime;
    std::mutex runtimeMutex;
    jcut::RuntimeControlServer controlServer;
    std::string documentPath;
    std::string projectId;
    std::string statePath;
    std::string historyPath;
    std::string projectRootPath;
    std::string lastSavedSnapshotJson;
    std::string statusMessage;
    AudioEngine audioEngine;
    bool audioInitialized = false;
    bool audioTimelineConfigured = false;
    bool audioPlaybackActive = false;
    int audioLastFrame = -1;
    double audioLastSpeed = 1.0;
    std::string audioTimelineSignature;
    std::string audioStatusMessage;
    nlohmann::json legacyStateRoot;
    bool usesQtProjectStorage = false;
    bool saveShortcutPressed = false;
    bool reloadShortcutPressed = false;
    bool fontSizeIncreaseShortcutPressed = false;
    bool fontSizeDecreaseShortcutPressed = false;
    float uiFontSize = kDefaultUiFontSize;
    float loadedUiFontSize = kDefaultUiFontSize;
    std::string preferencesPath;
    std::string uiFontPath;
    std::array<char, 512> importMediaPath{};
    std::array<char, 128> importMediaLabel{};
    std::array<char, 64> importMediaKind{};
    std::array<char, 512> mediaRootPath{};
    std::array<char, 128> mediaBrowserFilter{};
    std::string mediaGalleryPath;
    std::string mediaHoveredPath;
    std::string mediaSelectedPath;
    std::array<char, 512> exportOutputPath{};
    TimelineDragMode timelineDragMode = TimelineDragMode::None;
    int timelineDragClipId = 0;
    int timelineDragTrackId = 0;
    int timelineDragTrackIndex = -1;
    int timelineDragStartFrame = 0;
    int timelineDragDurationFrames = 0;
    float timelineDragMouseX = 0.0f;
    float timelineDragMouseY = 0.0f;
    std::mutex previewMutex;
    std::condition_variable previewCondition;
    std::thread previewWorker;
    bool previewStopRequested = false;
    bool previewRenderRequested = false;
    std::uint64_t previewRequestGeneration = 0;
    std::uint64_t previewCompletedGeneration = 0;
    std::uint64_t previewUploadedGeneration = 0;
    float previewPanX = 0.0f;
    float previewPanY = 0.0f;
    jcut::EditorDocumentCore previewDocument;
    std::string previewRootDirectory;
    jcut::standalone_render::PreviewRenderResult previewResult;
    ImTextureID previewTextureId = 0;
    std::mutex exportMutex;
    std::condition_variable exportCondition;
    std::thread exportWorker;
    bool exportStopRequested = false;
    bool exportRequested = false;
    bool exportRunning = false;
    bool exportCancelRequested = false;
    bool exportHasProgress = false;
    std::uint64_t exportRequestGeneration = 0;
    std::uint64_t exportCompletedGeneration = 0;
    std::uint64_t exportStatusGeneration = 0;
    jcut::EditorDocumentCore exportDocument;
    std::string exportRootDirectory;
    jcut::render::RenderProgressCore exportProgress;
    jcut::render::RenderResultCore exportResult;
};

struct PanelLayout {
    ImVec2 pos;
    ImVec2 size;
};

struct ShellLayout {
    PanelLayout media;
    PanelLayout preview;
    PanelLayout inspector;
    PanelLayout timeline;
};

std::string pathString(const fs::path& path)
{
    return path.lexically_normal().string();
}

std::string resolvePathForRoot(const std::string& path, const std::string& rootDirectory)
{
    if (path.empty()) {
        return {};
    }
    fs::path resolved(path);
    if (resolved.is_relative() && !rootDirectory.empty()) {
        resolved = fs::path(rootDirectory) / resolved;
    }
    return pathString(resolved);
}

std::string lowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string mediaKindForPath(const fs::path& path)
{
    const std::string ext = lowerAscii(path.extension().string());
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
        ext == ".webp" || ext == ".bmp" || ext == ".tif" || ext == ".tiff") {
        return "image";
    }
    if (ext == ".wav" || ext == ".mp3" || ext == ".flac" ||
        ext == ".aac" || ext == ".m4a" || ext == ".ogg") {
        return "audio";
    }
    if (ext == ".mp4" || ext == ".mov" || ext == ".mkv" ||
        ext == ".webm" || ext == ".avi" || ext == ".m4v") {
        return "video";
    }
    return "media";
}

bool isMediaFilePath(const fs::path& path)
{
    return mediaKindForPath(path) != "media";
}

std::string displayNameForPath(const fs::path& path)
{
    const std::string stem = path.stem().string();
    if (!stem.empty()) {
        return stem;
    }
    return path.filename().string();
}

jcut::EditorDocumentCore documentWithResolvedMediaPaths(
    jcut::EditorDocumentCore document,
    const std::string& rootDirectory)
{
    for (jcut::EditorMediaItem& mediaItem : document.mediaItems) {
        mediaItem.id = resolvePathForRoot(mediaItem.id, rootDirectory);
    }
    for (jcut::EditorClip& clip : document.clips) {
        clip.sourcePath = resolvePathForRoot(clip.sourcePath, rootDirectory);
    }
    return document;
}

template <typename T>
QVector<T> toQVector(const std::vector<T>& values)
{
    QVector<T> result;
    result.reserve(static_cast<qsizetype>(values.size()));
    for (const T& value : values) {
        result.push_back(value);
    }
    return result;
}

std::string readTextFile(const fs::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(stream)),
                       std::istreambuf_iterator<char>());
}

bool writeTextFileAtomically(const fs::path& path, const std::string& content)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    const fs::path tempPath = path.string() + ".tmp";
    {
        std::ofstream output(tempPath, std::ios::out | std::ios::trunc | std::ios::binary);
        if (!output.is_open()) {
            return false;
        }
        output << content;
        if (!output.good()) {
            return false;
        }
    }
    fs::rename(tempPath, path, ec);
    if (ec) {
        fs::remove(path, ec);
        ec.clear();
        fs::rename(tempPath, path, ec);
    }
    return !ec;
}

fs::path executableDirPath()
{
    std::error_code ec;
#if defined(__linux__)
    const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (!ec && !exe.empty()) {
        return exe.parent_path();
    }
#endif
    return fs::current_path();
}

std::vector<fs::path> uiFontCandidates()
{
    const fs::path exeDir = executableDirPath();
    std::vector<fs::path> candidates{
#if defined(__APPLE__)
        "/System/Library/Fonts/Helvetica.ttc",
        "/System/Library/Fonts/Supplemental/HelveticaNeue.ttc",
#else
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
#endif
        exeDir / "assets" / "fonts" / "Roboto-Medium.ttf",
        exeDir / "external" / "imgui" / "misc" / "fonts" / "Roboto-Medium.ttf",
        fs::current_path() / "assets" / "fonts" / "Roboto-Medium.ttf",
        fs::current_path() / "external" / "imgui" / "misc" / "fonts" / "Roboto-Medium.ttf",
    };

    fs::path walker = exeDir;
    for (int i = 0; i < 4 && walker.has_parent_path(); ++i) {
        candidates.push_back(walker / "external" / "imgui" / "misc" / "fonts" / "Roboto-Medium.ttf");
        walker = walker.parent_path();
    }
    return candidates;
}

std::optional<fs::path> firstExistingPath(const std::vector<fs::path>& candidates)
{
    std::error_code ec;
    for (const fs::path& candidate : candidates) {
        if (fs::exists(candidate, ec) && fs::is_regular_file(candidate, ec)) {
            return candidate;
        }
        ec.clear();
    }
    return std::nullopt;
}

void loadUiPreferences(ShellState* shellState)
{
    if (shellState->preferencesPath.empty()) {
        return;
    }
    const std::string bytes = readTextFile(shellState->preferencesPath);
    if (bytes.empty()) {
        return;
    }
    try {
        const nlohmann::json root = nlohmann::json::parse(bytes);
        shellState->uiFontSize = std::clamp(
            root.value("uiFontSize", kDefaultUiFontSize),
            kMinUiFontSize,
            kMaxUiFontSize);
    } catch (...) {
        shellState->uiFontSize = kDefaultUiFontSize;
    }
}

void saveUiPreferences(const ShellState& shellState)
{
    if (shellState.preferencesPath.empty()) {
        return;
    }
    const nlohmann::json root{
        {"uiFontSize", shellState.uiFontSize}
    };
    writeTextFileAtomically(shellState.preferencesPath, root.dump(2) + "\n");
}

void applyUiFontScale(const ShellState& shellState)
{
    ImGuiIO& io = ImGui::GetIO();
    io.FontGlobalScale = shellState.loadedUiFontSize > 0.0f
        ? std::clamp(shellState.uiFontSize / shellState.loadedUiFontSize, 0.5f, 3.0f)
        : 1.0f;
}

void loadUiFont(ShellState* shellState)
{
    ImGuiIO& io = ImGui::GetIO();
    const std::optional<fs::path> fontPath = firstExistingPath(uiFontCandidates());
    if (fontPath.has_value()) {
        ImFontConfig config;
        config.OversampleH = 2;
        config.OversampleV = 2;
        ImFont* font = io.Fonts->AddFontFromFileTTF(
            pathString(*fontPath).c_str(),
            shellState->uiFontSize,
            &config);
        if (font) {
            io.FontDefault = font;
            shellState->loadedUiFontSize = shellState->uiFontSize;
            shellState->uiFontPath = pathString(*fontPath);
            applyUiFontScale(*shellState);
            return;
        }
    }

    io.Fonts->AddFontDefault();
    shellState->loadedUiFontSize = kDefaultUiFontSize;
    applyUiFontScale(*shellState);
}

void changeUiFontSize(ShellState* shellState, float delta)
{
    const float nextSize = std::clamp(shellState->uiFontSize + delta, kMinUiFontSize, kMaxUiFontSize);
    if (std::abs(nextSize - shellState->uiFontSize) < 0.001f) {
        return;
    }
    shellState->uiFontSize = nextSize;
    applyUiFontScale(*shellState);
    saveUiPreferences(*shellState);
    shellState->statusMessage = "UI font size " + std::to_string(static_cast<int>(std::lround(nextSize))) + "px";
}

void requestPreviewRender(ShellState* shellState)
{
    jcut::EditorDocumentCore snapshot;
    {
        std::lock_guard<std::mutex> runtimeLock(shellState->runtimeMutex);
        snapshot = shellState->runtime.snapshot();
    }
    {
        std::lock_guard<std::mutex> lock(shellState->previewMutex);
        shellState->previewDocument = snapshot;
        shellState->previewRootDirectory = shellState->projectRootPath;
        shellState->previewRenderRequested = true;
        ++shellState->previewRequestGeneration;
    }
    shellState->previewCondition.notify_one();
}

bool requestExportRender(ShellState* shellState)
{
    jcut::EditorDocumentCore snapshot;
    {
        std::lock_guard<std::mutex> runtimeLock(shellState->runtimeMutex);
        snapshot = shellState->runtime.snapshot();
    }
    std::lock_guard<std::mutex> lock(shellState->exportMutex);
    if (shellState->exportRunning || shellState->exportRequested) {
        return false;
    }
    shellState->exportDocument = snapshot;
    shellState->exportRootDirectory = shellState->projectRootPath;
    shellState->exportRequested = true;
    shellState->exportCancelRequested = false;
    shellState->exportHasProgress = false;
    shellState->exportProgress = {};
    shellState->exportResult = {};
    ++shellState->exportRequestGeneration;
    shellState->exportCondition.notify_one();
    return true;
}

void cancelExportRender(ShellState* shellState)
{
    std::lock_guard<std::mutex> lock(shellState->exportMutex);
    shellState->exportCancelRequested = true;
}

std::string snapshotJson(const jcut::EditorDocumentCore& snapshot);

void configureAudioTimeline(ShellState* shellState, const jcut::EditorDocumentCore& snapshot)
{
    jcut::EditorDocumentCore signatureDocument = snapshot;
    signatureDocument.transport = {};
    const std::string signature =
        shellState->projectRootPath + "\n" + snapshotJson(signatureDocument);
    if (shellState->audioTimelineConfigured &&
        shellState->audioTimelineSignature == signature) {
        return;
    }

    const jcut::EditorDocumentCore renderDocument =
        documentWithResolvedMediaPaths(snapshot, shellState->projectRootPath);
    const jcut::render::TimelineRenderData timelineData =
        jcut::render::buildTimelineRenderData(renderDocument);
    shellState->audioEngine.setTimelineTracks(toQVector(timelineData.tracks));
    shellState->audioEngine.setTimelineClips(toQVector(timelineData.clips));
    shellState->audioEngine.setExportRanges(toQVector(timelineData.exportRanges));
    shellState->audioEngine.setRenderSyncMarkers(toQVector(timelineData.renderSyncMarkers));
    shellState->audioTimelineConfigured = true;
    shellState->audioTimelineSignature = signature;
}

bool ensureAudioInitialized(ShellState* shellState)
{
    if (shellState->audioInitialized) {
        return true;
    }
    shellState->audioInitialized = shellState->audioEngine.initialize();
    if (!shellState->audioInitialized) {
        shellState->audioStatusMessage =
            shellState->audioEngine.audioOutputStatusText().toStdString();
        if (shellState->audioStatusMessage.empty()) {
            shellState->audioStatusMessage = "audio output unavailable";
        }
    }
    return shellState->audioInitialized;
}

void syncAudioEngine(ShellState* shellState, const jcut::EditorDocumentCore& snapshot)
{
    if (!snapshot.transport.playbackActive) {
        if (shellState->audioPlaybackActive) {
            shellState->audioEngine.stop();
        }
        shellState->audioPlaybackActive = false;
        shellState->audioLastFrame = snapshot.transport.currentFrame;
        return;
    }

    if (!ensureAudioInitialized(shellState)) {
        return;
    }

    configureAudioTimeline(shellState, snapshot);
    const double speed = snapshot.transport.playbackSpeed == 0.0
        ? 1.0
        : snapshot.transport.playbackSpeed;
    const PlaybackAudioWarpMode warpMode =
        std::abs(speed - 1.0) < 0.0001
            ? PlaybackAudioWarpMode::Disabled
            : PlaybackAudioWarpMode::Varispeed;
    shellState->audioEngine.setPlaybackWarpMode(
        normalizedPlaybackAudioWarpMode(speed, warpMode));
    shellState->audioEngine.setPlaybackRate(
        effectivePlaybackAudioWarpRate(speed, warpMode));

    if (!shellState->audioEngine.hasPlayableAudio()) {
        if (shellState->audioPlaybackActive) {
            shellState->audioEngine.stop();
        }
        shellState->audioPlaybackActive = false;
        shellState->audioStatusMessage = "no playable audio on timeline";
        return;
    }

    const int currentFrame = snapshot.transport.currentFrame;
    const bool speedChanged = std::abs(speed - shellState->audioLastSpeed) >= 0.0001;
    const bool frameJumped = shellState->audioLastFrame >= 0 &&
        std::abs(currentFrame - shellState->audioLastFrame) > 8;

    if (!shellState->audioPlaybackActive || !shellState->audioEngine.playbackStarted()) {
        if (!shellState->audioEngine.warmPlaybackAudio(currentFrame, 1000)) {
            std::fprintf(stderr,
                         "[AUDIO WARN] continuing playback without warmed audio at frame %d\n",
                         currentFrame);
        }
        shellState->audioEngine.start(currentFrame);
        shellState->audioPlaybackActive = true;
    } else if (frameJumped || speedChanged) {
        shellState->audioEngine.seek(currentFrame);
    }

    shellState->audioLastFrame = currentFrame;
    shellState->audioLastSpeed = speed;
    shellState->audioStatusMessage =
        shellState->audioEngine.audioOutputStatusText().toStdString();
}

template <typename Command>
void applyCommand(ShellState* shellState, Command&& command)
{
    const jcut::CommandResult result =
        [&]() {
            std::lock_guard<std::mutex> lock(shellState->runtimeMutex);
            return shellState->runtime.execute(jcut::EditorCommand{std::forward<Command>(command)});
        }();
    shellState->statusMessage = result.message;
    requestPreviewRender(shellState);
}

int selectedTrackId(const jcut::EditorDocumentCore& snapshot)
{
    for (const jcut::EditorTrack& track : snapshot.tracks) {
        if (track.selected) {
            return track.id;
        }
    }
    return snapshot.tracks.empty() ? 0 : snapshot.tracks.front().id;
}

const jcut::EditorClip* selectedClip(const jcut::EditorDocumentCore& snapshot)
{
    for (const jcut::EditorClip& clip : snapshot.clips) {
        if (clip.selected) {
            return &clip;
        }
    }
    return nullptr;
}

const jcut::EditorTrack* selectedTrack(const jcut::EditorDocumentCore& snapshot)
{
    for (const jcut::EditorTrack& track : snapshot.tracks) {
        if (track.selected) {
            return &track;
        }
    }
    return nullptr;
}

void importFilesystemMedia(ShellState* shellState,
                           const jcut::EditorDocumentCore& snapshot,
                           const fs::path& path,
                           bool insertOnTimeline)
{
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        return;
    }
    const std::string normalizedPath = pathString(path);
    const std::string label = displayNameForPath(path);
    const std::string kind = mediaKindForPath(path);
    applyCommand(shellState, jcut::ImportMediaCommand{normalizedPath, label, kind});
    std::snprintf(shellState->importMediaPath.data(),
                  shellState->importMediaPath.size(),
                  "%s",
                  normalizedPath.c_str());
    std::snprintf(shellState->importMediaLabel.data(),
                  shellState->importMediaLabel.size(),
                  "%s",
                  label.c_str());
    std::snprintf(shellState->importMediaKind.data(),
                  shellState->importMediaKind.size(),
                  "%s",
                  kind.c_str());

    if (insertOnTimeline) {
        const int trackId = selectedTrackId(snapshot);
        if (trackId > 0) {
            applyCommand(shellState, jcut::InsertClipFromMediaCommand{
                normalizedPath,
                trackId,
                snapshot.transport.currentFrame,
                90});
        }
    }
}

std::vector<fs::directory_entry> sortedDirectoryEntries(const fs::path& root,
                                                       const std::string& filter)
{
    std::vector<fs::directory_entry> entries;
    std::error_code ec;
    if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) {
        return entries;
    }
    const std::string normalizedFilter = lowerAscii(filter);
    for (const fs::directory_entry& entry : fs::directory_iterator(root, ec)) {
        if (ec) {
            break;
        }
        const std::string name = entry.path().filename().string();
        if (!normalizedFilter.empty() &&
            lowerAscii(name).find(normalizedFilter) == std::string::npos) {
            continue;
        }
        entries.push_back(entry);
    }
    std::sort(entries.begin(), entries.end(), [](const fs::directory_entry& a,
                                                 const fs::directory_entry& b) {
        std::error_code ecA;
        std::error_code ecB;
        const bool aDir = a.is_directory(ecA);
        const bool bDir = b.is_directory(ecB);
        if (aDir != bDir) {
            return aDir > bDir;
        }
        return lowerAscii(a.path().filename().string()) <
               lowerAscii(b.path().filename().string());
    });
    return entries;
}

std::string snapshotJson(const jcut::EditorDocumentCore& snapshot)
{
    return jcut::toJson(snapshot).dump();
}

jcut::RuntimeControlSnapshot runtimeControlSnapshot(ShellState* shellState)
{
    jcut::EditorDocumentCore document;
    {
        std::lock_guard<std::mutex> lock(shellState->runtimeMutex);
        document = shellState->runtime.snapshot();
    }

    jcut::standalone_render::PreviewRenderResult previewResult;
    std::uint64_t previewCompletedGeneration = 0;
    {
        std::lock_guard<std::mutex> lock(shellState->previewMutex);
        previewResult = shellState->previewResult;
        previewCompletedGeneration = shellState->previewCompletedGeneration;
    }

    jcut::render::RenderProgressCore exportProgress;
    jcut::render::RenderResultCore exportResult;
    bool exportRunning = false;
    bool exportHasProgress = false;
    std::uint64_t exportCompletedGeneration = 0;
    {
        std::lock_guard<std::mutex> lock(shellState->exportMutex);
        exportProgress = shellState->exportProgress;
        exportResult = shellState->exportResult;
        exportRunning = shellState->exportRunning;
        exportHasProgress = shellState->exportHasProgress;
        exportCompletedGeneration = shellState->exportCompletedGeneration;
    }

    nlohmann::json renderStatus{
        {"ok", true},
        {"backend", "standalone"},
        {"path", exportResult.usedGpu ? "gpu" : "cpu_fallback"},
        {"usingGpu", exportResult.usedGpu},
        {"usedHardwareEncode", exportResult.usedHardwareEncode},
        {"encoder", exportResult.encoderLabel},
        {"exportRunning", exportRunning},
        {"exportHasProgress", exportHasProgress},
        {"exportCompletedGeneration", exportCompletedGeneration},
        {"lastRenderProgress", jcut::render::toJson(exportProgress)},
        {"lastRenderResult", jcut::render::toJson(exportResult)},
        {"audio", {
            {"initialized", shellState->audioInitialized},
            {"timelineConfigured", shellState->audioTimelineConfigured},
            {"playbackActive", shellState->audioPlaybackActive},
            {"playbackStarted", shellState->audioInitialized
                ? shellState->audioEngine.playbackStarted()
                : false},
            {"hasPlayableAudio", shellState->audioInitialized
                ? shellState->audioEngine.hasPlayableAudio()
                : false},
            {"clockAvailable", shellState->audioInitialized
                ? shellState->audioEngine.audioClockAvailable()
                : false},
            {"outputUnavailable", shellState->audioInitialized
                ? shellState->audioEngine.audioOutputUnavailableForPlayback()
                : false},
            {"status", shellState->audioStatusMessage}
        }},
        {"preview", {
            {"generation", previewCompletedGeneration},
            {"success", previewResult.success},
            {"message", previewResult.message},
            {"sourcePath", previewResult.sourcePath},
            {"zeroCopyVulkan", {
                {"ready", previewResult.vulkanFrame.valid},
                {"presentedFrames", previewResult.vulkanFrame.valid ? previewCompletedGeneration : 0},
                {"failures", 0},
                {"failureReason", ""},
                {"lastFrameValid", previewResult.vulkanFrame.valid},
                {"lastFrameWidth", previewResult.vulkanFrame.size.width},
                {"lastFrameHeight", previewResult.vulkanFrame.size.height}
            }},
            {"image", {
                {"valid", !previewResult.image.empty()},
                {"width", previewResult.image.size.width},
                {"height", previewResult.image.size.height},
                {"strideBytes", previewResult.image.strideBytes},
                {"byteCount", previewResult.image.bytes.size()}
            }}
        }}
    };

    const nlohmann::json documentJson = jcut::toJson(document);
    nlohmann::json profile{
        {"backend", "imgui"},
        {"project", document.projectName},
        {"media_count", document.mediaItems.size()},
        {"track_count", document.tracks.size()},
        {"clip_count", document.clips.size()},
        {"transport", documentJson.value("transport", nlohmann::json::object())},
        {"exportRequest", jcut::render::toJson(document.exportRequest)},
        {"render", renderStatus}
    };

    return jcut::RuntimeControlSnapshot{
        documentJson,
        renderStatus,
        profile,
        previewResult.image
    };
}

jcut::core::ImageBuffer makeRuntimeDiagnosticImage(const jcut::EditorDocumentCore& document)
{
    constexpr int width = 960;
    constexpr int height = 540;
    jcut::core::ImageBuffer image;
    image.size = {width, height};
    image.strideBytes = width * 4;
    image.bytes.resize(static_cast<std::size_t>(image.strideBytes) * height);

    auto setPixel = [&](int x, int y, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a = 255) {
        if (x < 0 || y < 0 || x >= width || y >= height) {
            return;
        }
        const std::size_t offset = static_cast<std::size_t>(y * image.strideBytes + x * 4);
        image.bytes[offset + 0] = r;
        image.bytes[offset + 1] = g;
        image.bytes[offset + 2] = b;
        image.bytes[offset + 3] = a;
    };
    auto fillRect = [&](int x, int y, int w, int h, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        for (int yy = y; yy < y + h; ++yy) {
            for (int xx = x; xx < x + w; ++xx) {
                setPixel(xx, yy, r, g, b);
            }
        }
    };

    fillRect(0, 0, width, height, 12, 14, 17);
    fillRect(32, 32, width - 64, 310, 20, 24, 29);
    fillRect(36, 36, width - 72, 302, 43, 49, 59);
    fillRect(64, 370, width - 128, 88, 18, 21, 25);

    const int timelineStart = 92;
    const int timelineWidth = width - 184;
    const int timelineEnd = std::max(1, [&]() {
        int endFrame = 1;
        for (const jcut::EditorClip& clip : document.clips) {
            endFrame = std::max(endFrame, clip.startFrame + clip.durationFrames);
        }
        return endFrame;
    }());
    for (const jcut::EditorClip& clip : document.clips) {
        const int x = timelineStart + static_cast<int>(
            (static_cast<double>(clip.startFrame) / timelineEnd) * timelineWidth);
        const int w = std::max(3, static_cast<int>(
            (static_cast<double>(clip.durationFrames) / timelineEnd) * timelineWidth));
        fillRect(x, 398, std::min(w, width - x - 64), 26, 242, 177, 69);
    }

    const int playheadX = timelineStart + static_cast<int>(
        (static_cast<double>(document.transport.currentFrame) / timelineEnd) * timelineWidth);
    fillRect(std::clamp(playheadX, 64, width - 65), 370, 3, 88, 100, 190, 198);

    const int previewW = document.exportRequest.outputSize.valid() ? document.exportRequest.outputSize.width : 1080;
    const int previewH = document.exportRequest.outputSize.valid() ? document.exportRequest.outputSize.height : 1920;
    const double aspect = previewH > 0 ? static_cast<double>(previewW) / previewH : 1.0;
    int boxH = 260;
    int boxW = std::max(60, static_cast<int>(boxH * aspect));
    if (boxW > width - 160) {
        boxW = width - 160;
        boxH = std::max(60, static_cast<int>(boxW / std::max(0.1, aspect)));
    }
    const int boxX = (width - boxW) / 2;
    const int boxY = 58;
    fillRect(boxX, boxY, boxW, boxH, 51, 57, 68);
    fillRect(boxX, boxY, boxW, 3, 219, 132, 46);
    fillRect(boxX, boxY + boxH - 3, boxW, 3, 219, 132, 46);
    fillRect(boxX, boxY, 3, boxH, 219, 132, 46);
    fillRect(boxX + boxW - 3, boxY, 3, boxH, 219, 132, 46);

    return image;
}

jcut::core::ImageBuffer runtimeControlScreenshot(ShellState* shellState)
{
    jcut::standalone_render::PreviewRenderResult previewResult;
    {
        std::lock_guard<std::mutex> lock(shellState->previewMutex);
        previewResult = shellState->previewResult;
    }
    if (!previewResult.image.empty()) {
        return previewResult.image;
    }

    jcut::EditorDocumentCore document;
    {
        std::lock_guard<std::mutex> lock(shellState->runtimeMutex);
        document = shellState->runtime.snapshot();
    }
    return makeRuntimeDiagnosticImage(document);
}

bool setRuntimeControlPlayhead(ShellState* shellState, std::int64_t frame, std::string* error)
{
    if (frame < 0 || frame > static_cast<std::int64_t>(std::numeric_limits<int>::max())) {
        if (error) {
            *error = "invalid frame";
        }
        return false;
    }
    const jcut::CommandResult result =
        [&]() {
            std::lock_guard<std::mutex> lock(shellState->runtimeMutex);
            return shellState->runtime.execute(jcut::EditorCommand{
                jcut::SeekToFrameCommand{static_cast<int>(frame)}});
        }();
    if (!result.applied && error) {
        *error = result.message;
    }
    requestPreviewRender(shellState);
    return result.applied;
}

ShellLayout computeShellLayout()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float top = viewport->Pos.y + ImGui::GetFrameHeight() + kShellGap;
    const float bottom = viewport->Pos.y + viewport->Size.y - kStatusBarHeight - kShellGap;
    const float left = viewport->Pos.x + kShellGap;
    const float right = viewport->Pos.x + viewport->Size.x - kShellGap;
    const float contentHeight = std::max(400.0f, bottom - top);

    const float mediaWidth = std::min(kMediaPanelWidth, viewport->Size.x * 0.22f);
    const float inspectorWidth = std::min(kInspectorPanelWidth, viewport->Size.x * 0.24f);
    const float timelineHeight = std::min(kTimelinePanelHeight, contentHeight * 0.33f);
    const float centerLeft = left + mediaWidth + kShellGap;
    const float centerRight = right - inspectorWidth - kShellGap;
    const float centerWidth = std::max(480.0f, centerRight - centerLeft);
    const float topHeight = std::max(280.0f, contentHeight - timelineHeight - kShellGap);

    ShellLayout layout;
    layout.media = {{left, top}, {mediaWidth, contentHeight}};
    layout.preview = {{centerLeft, top}, {centerWidth, topHeight}};
    layout.inspector = {{centerRight + kShellGap, top}, {inspectorWidth, contentHeight}};
    layout.timeline = {{centerLeft, top + topHeight + kShellGap}, {centerWidth, timelineHeight}};
    return layout;
}

bool saveCurrentDocument(ShellState* shellState)
{
    jcut::EditorDocumentCore snapshot;
    {
        std::lock_guard<std::mutex> lock(shellState->runtimeMutex);
        snapshot = shellState->runtime.snapshot();
    }
    if (shellState->usesQtProjectStorage) {
        const jcut::ImGuiProjectSession session{
            snapshot,
            shellState->projectId,
            shellState->statePath,
            shellState->historyPath,
            shellState->projectRootPath,
            shellState->legacyStateRoot
        };
        std::string error;
        if (!jcut::saveImGuiProjectSession(session, snapshot, &error)) {
            shellState->statusMessage = error;
            return false;
        }
        shellState->legacyStateRoot = jcut::toLegacyStateJson(snapshot, &shellState->legacyStateRoot);
        shellState->lastSavedSnapshotJson = snapshotJson(snapshot);
        shellState->statusMessage = "project state saved";
        return true;
    }

    if (shellState->documentPath.empty()) {
        shellState->statusMessage = "save unavailable: no document path";
        return false;
    }

    std::string error;
    if (!jcut::saveEditorDocumentCoreToFile(snapshot, shellState->documentPath, &error)) {
        shellState->statusMessage = error;
        return false;
    }

    shellState->lastSavedSnapshotJson = snapshotJson(snapshot);
    shellState->statusMessage = "document saved";
    return true;
}

bool reloadCurrentDocument(ShellState* shellState)
{
    if (shellState->usesQtProjectStorage) {
        std::string error;
        const std::optional<jcut::ImGuiProjectSession> session =
            jcut::loadActiveImGuiProjectSession(&error);
        if (!session.has_value()) {
            shellState->statusMessage = error;
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(shellState->runtimeMutex);
            shellState->runtime = jcut::EditorRuntime::fromDocument(session->document);
        }
        shellState->projectId = session->projectId;
        shellState->statePath = session->statePath;
        shellState->historyPath = session->historyPath;
        shellState->projectRootPath = session->rootDirPath;
        shellState->legacyStateRoot = session->legacyStateRoot;
        shellState->lastSavedSnapshotJson = snapshotJson(session->document);
        std::snprintf(shellState->exportOutputPath.data(),
                      shellState->exportOutputPath.size(),
                      "%s",
                      session->document.exportRequest.outputPath.c_str());
        shellState->statusMessage = "active project reloaded";
        requestPreviewRender(shellState);
        return true;
    }

    if (shellState->documentPath.empty()) {
        shellState->statusMessage = "reload unavailable: no document path";
        return false;
    }

    std::string error;
    const std::optional<jcut::EditorDocumentCore> document =
        jcut::loadEditorDocumentCoreFromFile(shellState->documentPath, &error);
    if (!document.has_value()) {
        shellState->statusMessage = error;
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(shellState->runtimeMutex);
        shellState->runtime = jcut::EditorRuntime::fromDocument(*document);
    }
    shellState->lastSavedSnapshotJson = snapshotJson(*document);
    shellState->projectRootPath = pathString(fs::path(shellState->documentPath).parent_path());
    std::snprintf(shellState->exportOutputPath.data(),
                  shellState->exportOutputPath.size(),
                  "%s",
                  document->exportRequest.outputPath.c_str());
    shellState->statusMessage = "document reloaded";
    requestPreviewRender(shellState);
    return true;
}

bool documentIsDirty(const ShellState& shellState, const jcut::EditorDocumentCore& snapshot)
{
    if (shellState.documentPath.empty() && !shellState.usesQtProjectStorage) {
        return false;
    }
    return snapshotJson(snapshot) != shellState.lastSavedSnapshotJson;
}

void runPreviewWorker(ShellState* shellState)
{
    for (;;) {
        jcut::EditorDocumentCore document;
        std::string rootDirectory;
        std::uint64_t generation = 0;
        {
            std::unique_lock<std::mutex> lock(shellState->previewMutex);
            shellState->previewCondition.wait(lock, [shellState]() {
                return shellState->previewStopRequested || shellState->previewRenderRequested;
            });
            if (shellState->previewStopRequested) {
                return;
            }
            document = shellState->previewDocument;
            rootDirectory = shellState->previewRootDirectory;
            generation = shellState->previewRequestGeneration;
            shellState->previewRenderRequested = false;
        }

        const jcut::standalone_render::PreviewRenderResult result =
            jcut::standalone_render::renderPreviewFrame(
                jcut::standalone_render::PreviewRenderRequest{
                    document,
                    document.exportRequest.outputSize.valid()
                        ? document.exportRequest.outputSize
                        : jcut::core::SizeI{1080, 1920},
                    document.transport.currentFrame,
                    rootDirectory,
                    true});

        {
            std::lock_guard<std::mutex> lock(shellState->previewMutex);
            if (generation >= shellState->previewCompletedGeneration) {
                shellState->previewResult = result;
                shellState->previewCompletedGeneration = generation;
            }
        }
    }
}

void runExportWorker(ShellState* shellState)
{
    for (;;) {
        jcut::EditorDocumentCore document;
        std::string rootDirectory;
        std::uint64_t generation = 0;
        {
            std::unique_lock<std::mutex> lock(shellState->exportMutex);
            shellState->exportCondition.wait(lock, [shellState]() {
                return shellState->exportStopRequested || shellState->exportRequested;
            });
            if (shellState->exportStopRequested && !shellState->exportRequested) {
                return;
            }
            document = shellState->exportDocument;
            rootDirectory = shellState->exportRootDirectory;
            generation = shellState->exportRequestGeneration;
            shellState->exportRequested = false;
            shellState->exportRunning = true;
            shellState->exportHasProgress = false;
            shellState->exportProgress = {};
            shellState->exportResult = {};
        }

        const jcut::render::RenderResultCore result =
            jcut::standalone_render::exportTimelineToFile(
                jcut::standalone_render::ExportRenderRequest{document, rootDirectory},
                [shellState](const jcut::render::RenderProgressCore& progress) {
                    std::lock_guard<std::mutex> lock(shellState->exportMutex);
                    shellState->exportProgress = progress;
                    shellState->exportHasProgress = true;
                    return !shellState->exportCancelRequested && !shellState->exportStopRequested;
                });

        {
            std::lock_guard<std::mutex> lock(shellState->exportMutex);
            shellState->exportResult = result;
            shellState->exportRunning = false;
            shellState->exportCompletedGeneration = generation;
        }
    }
}

int frameFromTimelineX(float originX, float mouseX)
{
    const float relative = mouseX - (originX + kTimelineLabelWidth + kTimelineTrackPadding);
    return std::max(0, static_cast<int>(std::lround(relative / kTimelinePixelsPerFrame)));
}

int trackIndexFromTimelineY(const jcut::EditorDocumentCore& snapshot, float originY, float mouseY)
{
    const float relative = mouseY - (originY + kTimelineTopPadding);
    const int index = static_cast<int>(std::floor(relative / kTimelineRowHeight));
    if (index < 0 || index >= static_cast<int>(snapshot.tracks.size())) {
        return -1;
    }
    return index;
}

void clearTimelineDrag(ShellState* shellState)
{
    shellState->timelineDragMode = TimelineDragMode::None;
    shellState->timelineDragClipId = 0;
    shellState->timelineDragTrackId = 0;
    shellState->timelineDragTrackIndex = -1;
}

void applyShellStyle()
{
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 5.0f;
    style.GrabRounding = 5.0f;
    style.TabRounding = 6.0f;
    style.ScrollbarRounding = 6.0f;
    style.FramePadding = ImVec2(10.0f, 6.0f);
    style.ItemSpacing = ImVec2(8.0f, 8.0f);
    style.WindowPadding = ImVec2(10.0f, 10.0f);

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.09f, 0.10f, 1.0f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.10f, 0.11f, 0.13f, 1.0f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.13f, 0.15f, 0.98f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.17f, 0.20f, 1.0f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.22f, 0.24f, 0.27f, 1.0f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.27f, 0.30f, 0.34f, 1.0f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.10f, 0.11f, 0.13f, 1.0f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.13f, 0.14f, 0.17f, 1.0f);
    colors[ImGuiCol_MenuBarBg] = ImVec4(0.10f, 0.11f, 0.12f, 1.0f);
    colors[ImGuiCol_Header] = ImVec4(0.18f, 0.22f, 0.24f, 1.0f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.24f, 0.30f, 0.32f, 1.0f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.28f, 0.35f, 0.37f, 1.0f);
    colors[ImGuiCol_Button] = ImVec4(0.19f, 0.33f, 0.34f, 1.0f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.24f, 0.41f, 0.42f, 1.0f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.16f, 0.28f, 0.29f, 1.0f);
    colors[ImGuiCol_Tab] = ImVec4(0.13f, 0.14f, 0.16f, 1.0f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.22f, 0.24f, 0.27f, 1.0f);
    colors[ImGuiCol_TabActive] = ImVec4(0.18f, 0.20f, 0.23f, 1.0f);
    colors[ImGuiCol_Separator] = ImVec4(0.22f, 0.23f, 0.26f, 1.0f);
    colors[ImGuiCol_ResizeGrip] = ImVec4(0.25f, 0.28f, 0.30f, 0.6f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.91f, 0.53f, 0.24f, 1.0f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.91f, 0.53f, 0.24f, 0.9f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.99f, 0.64f, 0.30f, 1.0f);
}

VkClearValue makeVulkanClearValue()
{
    VkClearValue clearValue{};
    clearValue.color.float32[0] = 0.05f;
    clearValue.color.float32[1] = 0.06f;
    clearValue.color.float32[2] = 0.07f;
    clearValue.color.float32[3] = 1.0f;
    return clearValue;
}

bool hasVulkanExtension(const std::vector<VkExtensionProperties>& properties, const char* name)
{
    return std::any_of(properties.begin(), properties.end(), [&](const VkExtensionProperties& ext) {
        return std::strcmp(ext.extensionName, name) == 0;
    });
}

void checkVulkanResult(VkResult err)
{
    if (err != VK_SUCCESS && err != VK_SUBOPTIMAL_KHR) {
        std::fprintf(stderr, "jcut_imgui Vulkan backend error: %d\n", static_cast<int>(err));
    }
}

ImGuiKey imguiKeyFromX11KeySym(KeySym keySym)
{
    switch (keySym) {
    case XK_Tab: return ImGuiKey_Tab;
    case XK_Left: return ImGuiKey_LeftArrow;
    case XK_Right: return ImGuiKey_RightArrow;
    case XK_Up: return ImGuiKey_UpArrow;
    case XK_Down: return ImGuiKey_DownArrow;
    case XK_Page_Up: return ImGuiKey_PageUp;
    case XK_Page_Down: return ImGuiKey_PageDown;
    case XK_Home: return ImGuiKey_Home;
    case XK_End: return ImGuiKey_End;
    case XK_Insert: return ImGuiKey_Insert;
    case XK_Delete: return ImGuiKey_Delete;
    case XK_BackSpace: return ImGuiKey_Backspace;
    case XK_space: return ImGuiKey_Space;
    case XK_Return: return ImGuiKey_Enter;
    case XK_KP_Enter: return ImGuiKey_KeypadEnter;
    case XK_Escape: return ImGuiKey_Escape;
    case XK_apostrophe: return ImGuiKey_Apostrophe;
    case XK_comma: return ImGuiKey_Comma;
    case XK_minus: return ImGuiKey_Minus;
    case XK_period: return ImGuiKey_Period;
    case XK_slash: return ImGuiKey_Slash;
    case XK_semicolon: return ImGuiKey_Semicolon;
    case XK_equal: return ImGuiKey_Equal;
    case XK_bracketleft: return ImGuiKey_LeftBracket;
    case XK_backslash: return ImGuiKey_Backslash;
    case XK_bracketright: return ImGuiKey_RightBracket;
    case XK_grave: return ImGuiKey_GraveAccent;
    case XK_Caps_Lock: return ImGuiKey_CapsLock;
    case XK_Scroll_Lock: return ImGuiKey_ScrollLock;
    case XK_Num_Lock: return ImGuiKey_NumLock;
    case XK_Print: return ImGuiKey_PrintScreen;
    case XK_Pause: return ImGuiKey_Pause;
    case XK_KP_0: return ImGuiKey_Keypad0;
    case XK_KP_1: return ImGuiKey_Keypad1;
    case XK_KP_2: return ImGuiKey_Keypad2;
    case XK_KP_3: return ImGuiKey_Keypad3;
    case XK_KP_4: return ImGuiKey_Keypad4;
    case XK_KP_5: return ImGuiKey_Keypad5;
    case XK_KP_6: return ImGuiKey_Keypad6;
    case XK_KP_7: return ImGuiKey_Keypad7;
    case XK_KP_8: return ImGuiKey_Keypad8;
    case XK_KP_9: return ImGuiKey_Keypad9;
    case XK_KP_Decimal: return ImGuiKey_KeypadDecimal;
    case XK_KP_Divide: return ImGuiKey_KeypadDivide;
    case XK_KP_Multiply: return ImGuiKey_KeypadMultiply;
    case XK_KP_Subtract: return ImGuiKey_KeypadSubtract;
    case XK_KP_Add: return ImGuiKey_KeypadAdd;
    case XK_KP_Equal: return ImGuiKey_KeypadEqual;
    case XK_Control_L: return ImGuiKey_LeftCtrl;
    case XK_Shift_L: return ImGuiKey_LeftShift;
    case XK_Alt_L: return ImGuiKey_LeftAlt;
    case XK_Super_L: return ImGuiKey_LeftSuper;
    case XK_Control_R: return ImGuiKey_RightCtrl;
    case XK_Shift_R: return ImGuiKey_RightShift;
    case XK_Alt_R: return ImGuiKey_RightAlt;
    case XK_Super_R: return ImGuiKey_RightSuper;
    case XK_F1: return ImGuiKey_F1;
    case XK_F2: return ImGuiKey_F2;
    case XK_F3: return ImGuiKey_F3;
    case XK_F4: return ImGuiKey_F4;
    case XK_F5: return ImGuiKey_F5;
    case XK_F6: return ImGuiKey_F6;
    case XK_F7: return ImGuiKey_F7;
    case XK_F8: return ImGuiKey_F8;
    case XK_F9: return ImGuiKey_F9;
    case XK_F10: return ImGuiKey_F10;
    case XK_F11: return ImGuiKey_F11;
    case XK_F12: return ImGuiKey_F12;
    default:
        break;
    }
    if (keySym >= XK_0 && keySym <= XK_9) {
        return static_cast<ImGuiKey>(ImGuiKey_0 + (keySym - XK_0));
    }
    if (keySym >= XK_A && keySym <= XK_Z) {
        return static_cast<ImGuiKey>(ImGuiKey_A + (keySym - XK_A));
    }
    if (keySym >= XK_a && keySym <= XK_z) {
        return static_cast<ImGuiKey>(ImGuiKey_A + (keySym - XK_a));
    }
    return ImGuiKey_None;
}

struct X11Platform {
    Display* display = nullptr;
    int screen = 0;
    Window window = 0;
    Atom wmDeleteWindow = 0;
    bool closeRequested = false;
    int width = 1600;
    int height = 960;
    bool ctrlDown = false;
    bool sDown = false;
    bool rDown = false;
    bool equalDown = false;
    bool kpAddDown = false;
    bool minusDown = false;
    bool kpSubtractDown = false;
    std::chrono::steady_clock::time_point lastFrameTime = std::chrono::steady_clock::now();

    bool create(int initialWidth, int initialHeight, const char* title, std::string* error)
    {
        display = XOpenDisplay(nullptr);
        if (!display) {
            if (error) *error = "Failed to open X11 display.";
            return false;
        }
        screen = DefaultScreen(display);
        width = initialWidth;
        height = initialHeight;

        XSetWindowAttributes attrs{};
        attrs.event_mask = ExposureMask |
            StructureNotifyMask |
            KeyPressMask |
            KeyReleaseMask |
            ButtonPressMask |
            ButtonReleaseMask |
            PointerMotionMask |
            FocusChangeMask;
        window = XCreateWindow(display,
                               RootWindow(display, screen),
                               0,
                               0,
                               static_cast<unsigned int>(width),
                               static_cast<unsigned int>(height),
                               0,
                               CopyFromParent,
                               InputOutput,
                               CopyFromParent,
                               CWEventMask,
                               &attrs);
        if (!window) {
            if (error) *error = "Failed to create X11 window.";
            shutdown();
            return false;
        }
        XStoreName(display, window, title);
        wmDeleteWindow = XInternAtom(display, "WM_DELETE_WINDOW", False);
        XSetWMProtocols(display, window, &wmDeleteWindow, 1);
        XMapWindow(display, window);
        XFlush(display);
        return true;
    }

    void updateKeyState(KeySym keySym, bool down)
    {
        if (keySym == XK_Control_L || keySym == XK_Control_R) ctrlDown = down;
        if (keySym == XK_s || keySym == XK_S) sDown = down;
        if (keySym == XK_r || keySym == XK_R) rDown = down;
        if (keySym == XK_equal || keySym == XK_plus) equalDown = down;
        if (keySym == XK_KP_Add) kpAddDown = down;
        if (keySym == XK_minus || keySym == XK_underscore) minusDown = down;
        if (keySym == XK_KP_Subtract) kpSubtractDown = down;
    }

    void updateModifiers(unsigned int state)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.AddKeyEvent(ImGuiMod_Ctrl, (state & ControlMask) != 0 || ctrlDown);
        io.AddKeyEvent(ImGuiMod_Shift, (state & ShiftMask) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (state & Mod1Mask) != 0);
        io.AddKeyEvent(ImGuiMod_Super, (state & Mod4Mask) != 0);
    }

    void pollEvents()
    {
        ImGuiIO& io = ImGui::GetIO();
        while (display && XPending(display) > 0) {
            XEvent event{};
            XNextEvent(display, &event);
            switch (event.type) {
            case ClientMessage:
                if (static_cast<Atom>(event.xclient.data.l[0]) == wmDeleteWindow) {
                    closeRequested = true;
                }
                break;
            case ConfigureNotify:
                width = std::max(1, event.xconfigure.width);
                height = std::max(1, event.xconfigure.height);
                break;
            case MotionNotify:
                io.AddMousePosEvent(static_cast<float>(event.xmotion.x), static_cast<float>(event.xmotion.y));
                break;
            case ButtonPress:
            case ButtonRelease: {
                const bool down = event.type == ButtonPress;
                const unsigned int button = event.xbutton.button;
                if (button == Button1) io.AddMouseButtonEvent(0, down);
                if (button == Button2) io.AddMouseButtonEvent(2, down);
                if (button == Button3) io.AddMouseButtonEvent(1, down);
                if (down && button == Button4) io.AddMouseWheelEvent(0.0f, 1.0f);
                if (down && button == Button5) io.AddMouseWheelEvent(0.0f, -1.0f);
                break;
            }
            case FocusIn:
                io.AddFocusEvent(true);
                break;
            case FocusOut:
                io.AddFocusEvent(false);
                break;
            case KeyPress:
            case KeyRelease: {
                const bool down = event.type == KeyPress;
                KeySym keySym = NoSymbol;
                char text[32]{};
                const int textLength = XLookupString(&event.xkey, text, sizeof(text), &keySym, nullptr);
                updateModifiers(event.xkey.state);
                updateKeyState(keySym, down);
                const ImGuiKey imguiKey = imguiKeyFromX11KeySym(keySym);
                if (imguiKey != ImGuiKey_None) {
                    io.AddKeyEvent(imguiKey, down);
                }
                if (down && textLength > 0) {
                    io.AddInputCharactersUTF8(text);
                }
                break;
            }
            default:
                break;
            }
        }
    }

    void newFrame()
    {
        ImGuiIO& io = ImGui::GetIO();
        io.BackendPlatformName = "jcut_x11";
        io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
        io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
        const auto now = std::chrono::steady_clock::now();
        io.DeltaTime = std::max(1.0f / 1000.0f,
                                static_cast<float>(std::chrono::duration<double>(now - lastFrameTime).count()));
        lastFrameTime = now;
    }

    bool shouldClose() const { return closeRequested; }
    bool savePressed() const { return ctrlDown && sDown; }
    bool reloadPressed() const { return ctrlDown && rDown; }
    bool fontIncreasePressed() const { return ctrlDown && (equalDown || kpAddDown); }
    bool fontDecreasePressed() const { return ctrlDown && (minusDown || kpSubtractDown); }

    void shutdown()
    {
        if (display && window) {
            XDestroyWindow(display, window);
            window = 0;
        }
        if (display) {
            XCloseDisplay(display);
            display = nullptr;
        }
    }
};

struct VulkanShell {
    X11Platform* platform = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queueFamily = UINT32_MAX;
    VkQueue queue = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkSampler previewSampler = VK_NULL_HANDLE;
    ImGui_ImplVulkanH_Window windowData{};
    uint32_t minImageCount = 2;
    bool swapchainRebuild = false;
    jcut::vulkan_detector::VulkanDetectorFrameHandoff previewHandoff;
    VkDescriptorSet previewTextureSet = VK_NULL_HANDLE;
    VkImageView boundPreviewView = VK_NULL_HANDLE;
    VkImageLayout boundPreviewLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    bool queueSupportsPresent(VkPhysicalDevice candidate, uint32_t family) const
    {
        VkBool32 supported = VK_FALSE;
        if (vkGetPhysicalDeviceSurfaceSupportKHR(candidate, family, surface, &supported) != VK_SUCCESS) {
            return false;
        }
        return supported == VK_TRUE;
    }

    bool selectQueueFamily(VkPhysicalDevice candidate, uint32_t* familyOut) const
    {
        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        if (familyCount > 0) {
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());
        }
        for (uint32_t i = 0; i < familyCount; ++i) {
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && queueSupportsPresent(candidate, i)) {
                *familyOut = i;
                return true;
            }
        }
        return false;
    }

    bool createInstance(std::string* error)
    {
        uint32_t propertyCount = 0;
        if (vkEnumerateInstanceExtensionProperties(nullptr, &propertyCount, nullptr) != VK_SUCCESS) {
            if (error) *error = "Failed to enumerate Vulkan instance extensions.";
            return false;
        }
        std::vector<VkExtensionProperties> properties(propertyCount);
        if (propertyCount > 0 &&
            vkEnumerateInstanceExtensionProperties(nullptr, &propertyCount, properties.data()) != VK_SUCCESS) {
            if (error) *error = "Failed to read Vulkan instance extensions.";
            return false;
        }

        if (!hasVulkanExtension(properties, VK_KHR_SURFACE_EXTENSION_NAME) ||
            !hasVulkanExtension(properties, VK_KHR_XLIB_SURFACE_EXTENSION_NAME)) {
            if (error) *error = "Required Vulkan Xlib surface extensions are unavailable.";
            return false;
        }

        std::vector<const char*> extensions{
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_KHR_XLIB_SURFACE_EXTENSION_NAME,
        };
        if (hasVulkanExtension(properties, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)) {
            extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
        }
        VkInstanceCreateFlags flags = 0;
#ifdef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
        if (hasVulkanExtension(properties, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
            extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
#endif

        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "jcut-imgui";
        appInfo.apiVersion = VK_API_VERSION_1_1;

        VkInstanceCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        info.flags = flags;
        info.pApplicationInfo = &appInfo;
        info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        info.ppEnabledExtensionNames = extensions.data();
        if (vkCreateInstance(&info, nullptr, &instance) != VK_SUCCESS) {
            if (error) *error = "Failed to create Vulkan instance.";
            return false;
        }
        return true;
    }

    bool selectPhysicalDevice(std::string* error)
    {
        uint32_t count = 0;
        if (vkEnumeratePhysicalDevices(instance, &count, nullptr) != VK_SUCCESS || count == 0) {
            if (error) *error = "No Vulkan physical device available.";
            return false;
        }
        std::vector<VkPhysicalDevice> devices(count);
        if (vkEnumeratePhysicalDevices(instance, &count, devices.data()) != VK_SUCCESS) {
            if (error) *error = "Failed to enumerate Vulkan physical devices.";
            return false;
        }
        for (VkPhysicalDevice candidate : devices) {
            uint32_t family = UINT32_MAX;
            if (selectQueueFamily(candidate, &family)) {
                physicalDevice = candidate;
                queueFamily = family;
                return true;
            }
        }
        if (error) *error = "No Vulkan graphics queue can present to the X11 surface.";
        return false;
    }

    bool createDevice(std::string* error)
    {
        uint32_t extensionCount = 0;
        vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, nullptr);
        std::vector<VkExtensionProperties> properties(extensionCount);
        if (extensionCount > 0) {
            vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, properties.data());
        }
        auto optionalExtension = [&](const char* name, std::vector<const char*>* extensions) {
            if (hasVulkanExtension(properties, name)) {
                extensions->push_back(name);
            }
        };
        if (!hasVulkanExtension(properties, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
            if (error) *error = "Vulkan swapchain extension is unavailable.";
            return false;
        }
        std::vector<const char*> extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        optionalExtension(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME, &extensions);
#ifdef __linux__
        optionalExtension(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, &extensions);
#endif
        optionalExtension(VK_KHR_BIND_MEMORY_2_EXTENSION_NAME, &extensions);
        optionalExtension(VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME, &extensions);
        optionalExtension(VK_KHR_MAINTENANCE1_EXTENSION_NAME, &extensions);
        optionalExtension(VK_KHR_MAINTENANCE3_EXTENSION_NAME, &extensions);
#ifdef VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
        optionalExtension(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME, &extensions);
#endif

        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{};
        queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfo.queueFamilyIndex = queueFamily;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;

        VkDeviceCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        info.queueCreateInfoCount = 1;
        info.pQueueCreateInfos = &queueInfo;
        info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        info.ppEnabledExtensionNames = extensions.data();
        if (vkCreateDevice(physicalDevice, &info, nullptr, &device) != VK_SUCCESS) {
            if (error) *error = "Failed to create Vulkan device.";
            return false;
        }
        vkGetDeviceQueue(device, queueFamily, 0, &queue);
        return true;
    }

    bool createDescriptorPool(std::string* error)
    {
        const std::array<VkDescriptorPoolSize, 2> poolSizes{{
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE + 64},
            {VK_DESCRIPTOR_TYPE_SAMPLER, IMGUI_IMPL_VULKAN_MINIMUM_SAMPLER_POOL_SIZE + 16},
        }};
        VkDescriptorPoolCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        info.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
        info.pPoolSizes = poolSizes.data();
        for (const VkDescriptorPoolSize& size : poolSizes) {
            info.maxSets += size.descriptorCount;
        }
        if (vkCreateDescriptorPool(device, &info, nullptr, &descriptorPool) != VK_SUCCESS) {
            if (error) *error = "Failed to create Vulkan descriptor pool.";
            return false;
        }
        return true;
    }

    bool createPreviewSampler(std::string* error)
    {
        VkSamplerCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        info.magFilter = VK_FILTER_LINEAR;
        info.minFilter = VK_FILTER_LINEAR;
        info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.maxLod = 1.0f;
        if (vkCreateSampler(device, &info, nullptr, &previewSampler) != VK_SUCCESS) {
            if (error) *error = "Failed to create Vulkan preview sampler.";
            return false;
        }
        return true;
    }

    bool setupSwapchain(int width, int height, std::string* error)
    {
        windowData.Surface = surface;
        const VkFormat formats[] = {
            VK_FORMAT_B8G8R8A8_UNORM,
            VK_FORMAT_R8G8B8A8_UNORM,
            VK_FORMAT_B8G8R8_UNORM,
            VK_FORMAT_R8G8B8_UNORM,
        };
        windowData.SurfaceFormat =
            ImGui_ImplVulkanH_SelectSurfaceFormat(physicalDevice,
                                                  surface,
                                                  formats,
                                                  IM_ARRAYSIZE(formats),
                                                  VK_COLORSPACE_SRGB_NONLINEAR_KHR);
        const VkPresentModeKHR presentModes[] = {VK_PRESENT_MODE_FIFO_KHR};
        windowData.PresentMode =
            ImGui_ImplVulkanH_SelectPresentMode(physicalDevice,
                                                surface,
                                                presentModes,
                                                IM_ARRAYSIZE(presentModes));
        ImGui_ImplVulkanH_CreateOrResizeWindow(instance,
                                               physicalDevice,
                                               device,
                                               &windowData,
                                               queueFamily,
                                               nullptr,
                                               width,
                                               height,
                                               minImageCount,
                                               0);
        windowData.ClearValue = makeVulkanClearValue();
        if (windowData.RenderPass == VK_NULL_HANDLE) {
            if (error) *error = "Failed to create Vulkan swapchain render pass.";
            return false;
        }
        return true;
    }

    bool initialize(X11Platform* x11Platform, std::string* error)
    {
        platform = x11Platform;
        if (!createInstance(error)) return false;
        VkXlibSurfaceCreateInfoKHR surfaceInfo{};
        surfaceInfo.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
        surfaceInfo.dpy = platform ? platform->display : nullptr;
        surfaceInfo.window = platform ? platform->window : 0;
        if (vkCreateXlibSurfaceKHR(instance, &surfaceInfo, nullptr, &surface) != VK_SUCCESS) {
            if (error) *error = "Failed to create Vulkan Xlib window surface.";
            return false;
        }
        if (!selectPhysicalDevice(error) ||
            !createDevice(error) ||
            !createDescriptorPool(error) ||
            !createPreviewSampler(error)) {
            return false;
        }
        if (!platform) {
            if (error) *error = "X11 platform is unavailable.";
            return false;
        }
        if (!setupSwapchain(std::max(1, platform->width), std::max(1, platform->height), error)) {
            return false;
        }

        ImGui_ImplVulkan_InitInfo initInfo{};
        initInfo.Instance = instance;
        initInfo.PhysicalDevice = physicalDevice;
        initInfo.Device = device;
        initInfo.QueueFamily = queueFamily;
        initInfo.Queue = queue;
        initInfo.DescriptorPool = descriptorPool;
        initInfo.MinImageCount = minImageCount;
        initInfo.ImageCount = windowData.ImageCount;
        initInfo.PipelineInfoMain.RenderPass = windowData.RenderPass;
        initInfo.PipelineInfoMain.Subpass = 0;
        initInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        initInfo.CheckVkResultFn = checkVulkanResult;
        if (!ImGui_ImplVulkan_Init(&initInfo)) {
            if (error) *error = "Failed to initialize ImGui Vulkan backend.";
            return false;
        }
        std::string handoffError;
        if (!previewHandoff.initialize({physicalDevice, device, queue, queueFamily}, &handoffError)) {
            if (error) *error = handoffError;
            return false;
        }
        return true;
    }

    bool bindPreviewFrame(const render_detail::OffscreenVulkanFrame& frame, ImTextureID* textureOut)
    {
        if (!frame.valid) {
            return false;
        }
        std::string error;
        if (!previewHandoff.importOffscreenFrame(frame, &error)) {
            std::fprintf(stderr, "Vulkan preview import failed: %s\n", error.c_str());
            return false;
        }
        const jcut::vulkan_detector::VulkanExternalImage external = previewHandoff.externalImage();
        if (external.imageView == VK_NULL_HANDLE || !external.size.valid()) {
            return false;
        }
        if (previewTextureSet == VK_NULL_HANDLE ||
            boundPreviewView != external.imageView ||
            boundPreviewLayout != external.imageLayout) {
            if (previewTextureSet != VK_NULL_HANDLE) {
                ImGui_ImplVulkan_RemoveTexture(previewTextureSet);
                previewTextureSet = VK_NULL_HANDLE;
            }
            previewTextureSet =
                ImGui_ImplVulkan_AddTexture(previewSampler, external.imageView, external.imageLayout);
            boundPreviewView = external.imageView;
            boundPreviewLayout = external.imageLayout;
        }
        if (textureOut) {
            *textureOut = reinterpret_cast<ImTextureID>(previewTextureSet);
        }
        return previewTextureSet != VK_NULL_HANDLE;
    }

    void rebuildSwapchainIfNeeded()
    {
        if (!platform || platform->width <= 0 || platform->height <= 0) {
            return;
        }
        if (swapchainRebuild ||
            windowData.Width != static_cast<uint32_t>(platform->width) ||
            windowData.Height != static_cast<uint32_t>(platform->height)) {
            vkDeviceWaitIdle(device);
            ImGui_ImplVulkan_SetMinImageCount(minImageCount);
            ImGui_ImplVulkanH_CreateOrResizeWindow(instance,
                                                   physicalDevice,
                                                   device,
                                                   &windowData,
                                                   queueFamily,
                                                   nullptr,
                                                   platform->width,
                                                   platform->height,
                                                   minImageCount,
                                                   0);
            windowData.ClearValue = makeVulkanClearValue();
            windowData.FrameIndex = 0;
            swapchainRebuild = false;
        }
    }

    void renderDrawData(ImDrawData* drawData)
    {
        ImGui_ImplVulkanH_Window* wd = &windowData;
        VkSemaphore imageAcquiredSemaphore = wd->FrameSemaphores[wd->SemaphoreIndex].ImageAcquiredSemaphore;
        VkSemaphore renderCompleteSemaphore = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
        VkResult err = vkAcquireNextImageKHR(device,
                                             wd->Swapchain,
                                             UINT64_MAX,
                                             imageAcquiredSemaphore,
                                             VK_NULL_HANDLE,
                                             &wd->FrameIndex);
        if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
            swapchainRebuild = true;
        }
        if (err == VK_ERROR_OUT_OF_DATE_KHR) {
            return;
        }
        checkVulkanResult(err);

        ImGui_ImplVulkanH_Frame* fd = &wd->Frames[wd->FrameIndex];
        vkWaitForFences(device, 1, &fd->Fence, VK_TRUE, UINT64_MAX);
        vkResetFences(device, 1, &fd->Fence);
        vkResetCommandPool(device, fd->CommandPool, 0);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(fd->CommandBuffer, &beginInfo);

        VkRenderPassBeginInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass = wd->RenderPass;
        renderPassInfo.framebuffer = fd->Framebuffer;
        renderPassInfo.renderArea.extent.width = wd->Width;
        renderPassInfo.renderArea.extent.height = wd->Height;
        renderPassInfo.clearValueCount = 1;
        renderPassInfo.pClearValues = &wd->ClearValue;
        vkCmdBeginRenderPass(fd->CommandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
        ImGui_ImplVulkan_RenderDrawData(drawData, fd->CommandBuffer);
        vkCmdEndRenderPass(fd->CommandBuffer);
        vkEndCommandBuffer(fd->CommandBuffer);

        const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = &imageAcquiredSemaphore;
        submitInfo.pWaitDstStageMask = &waitStage;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &fd->CommandBuffer;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &renderCompleteSemaphore;
        checkVulkanResult(vkQueueSubmit(queue, 1, &submitInfo, fd->Fence));
    }

    void present()
    {
        if (swapchainRebuild) {
            return;
        }
        ImGui_ImplVulkanH_Window* wd = &windowData;
        VkSemaphore renderCompleteSemaphore = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &renderCompleteSemaphore;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &wd->Swapchain;
        presentInfo.pImageIndices = &wd->FrameIndex;
        VkResult err = vkQueuePresentKHR(queue, &presentInfo);
        if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
            swapchainRebuild = true;
        } else {
            checkVulkanResult(err);
        }
        wd->SemaphoreIndex = (wd->SemaphoreIndex + 1) % wd->SemaphoreCount;
    }

    void shutdown()
    {
        if (device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device);
        }
        if (previewTextureSet != VK_NULL_HANDLE) {
            ImGui_ImplVulkan_RemoveTexture(previewTextureSet);
            previewTextureSet = VK_NULL_HANDLE;
        }
        previewHandoff.release();
        ImGui_ImplVulkan_Shutdown();
        if (device != VK_NULL_HANDLE && windowData.Surface != VK_NULL_HANDLE) {
            ImGui_ImplVulkanH_DestroyWindow(instance, device, &windowData, nullptr);
        }
        if (previewSampler != VK_NULL_HANDLE) {
            vkDestroySampler(device, previewSampler, nullptr);
            previewSampler = VK_NULL_HANDLE;
        }
        if (descriptorPool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device, descriptorPool, nullptr);
            descriptorPool = VK_NULL_HANDLE;
        }
        if (device != VK_NULL_HANDLE) {
            vkDestroyDevice(device, nullptr);
            device = VK_NULL_HANDLE;
        }
        if (surface != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance, surface, nullptr);
            surface = VK_NULL_HANDLE;
        }
        if (instance != VK_NULL_HANDLE) {
            vkDestroyInstance(instance, nullptr);
            instance = VK_NULL_HANDLE;
        }
    }
};

void uploadPreviewTexture(ShellState* shellState, VulkanShell* vulkanShell)
{
    jcut::standalone_render::PreviewRenderResult previewResult;
    std::uint64_t completedGeneration = 0;
    {
        std::lock_guard<std::mutex> lock(shellState->previewMutex);
        if (shellState->previewCompletedGeneration == 0 ||
            shellState->previewCompletedGeneration == shellState->previewUploadedGeneration) {
            return;
        }
        previewResult = shellState->previewResult;
        completedGeneration = shellState->previewCompletedGeneration;
    }

    if (vulkanShell && previewResult.vulkanFrame.valid) {
        ImTextureID texture = 0;
        if (vulkanShell->bindPreviewFrame(previewResult.vulkanFrame, &texture)) {
            shellState->previewTextureId = texture;
        }
    }

    {
        std::lock_guard<std::mutex> lock(shellState->previewMutex);
        shellState->previewUploadedGeneration = completedGeneration;
    }
}

void drawMenuBar(ShellState* shellState, const jcut::EditorDocumentCore& snapshot)
{
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }
    if (ImGui::BeginMenu("File")) {
        ImGui::MenuItem("Open Project", nullptr, false, false);
        ImGui::MenuItem("Import Media", nullptr, false, false);
        if (ImGui::MenuItem("Save", "Ctrl+S", false,
                            !shellState->documentPath.empty() || shellState->usesQtProjectStorage)) {
            saveCurrentDocument(shellState);
        }
        if (ImGui::MenuItem("Reload", "Ctrl+R", false,
                            !shellState->documentPath.empty() || shellState->usesQtProjectStorage)) {
            reloadCurrentDocument(shellState);
        }
        ImGui::Separator();
        ImGui::MenuItem("Export", nullptr, false, false);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Transport")) {
        bool playing = snapshot.transport.playbackActive;
        if (ImGui::MenuItem("Play", nullptr, playing, true)) {
            applyCommand(shellState, jcut::TogglePlaybackCommand{});
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        bool waveform = snapshot.panels.showWaveform;
        bool transcript = snapshot.panels.showTranscript;
        bool scopes = snapshot.panels.showScopes;
        if (ImGui::MenuItem("Waveform", nullptr, waveform, true)) {
            applyCommand(shellState, jcut::SetWaveformVisibleCommand{!waveform});
        }
        if (ImGui::MenuItem("Transcript", nullptr, transcript, true)) {
            applyCommand(shellState, jcut::SetTranscriptVisibleCommand{!transcript});
        }
        if (ImGui::MenuItem("Scopes", nullptr, scopes, true)) {
            applyCommand(shellState, jcut::SetScopesVisibleCommand{!scopes});
        }
        ImGui::EndMenu();
    }
    ImGui::Separator();
    const bool dirty = documentIsDirty(*shellState, snapshot);
    ImGui::Text("%s%s", snapshot.projectName.c_str(), dirty ? " *" : "");
    ImGui::EndMainMenuBar();
}

void drawMediaPanel(ShellState* shellState, const jcut::EditorDocumentCore& snapshot)
{
    const ShellLayout layout = computeShellLayout();
    ImGui::SetNextWindowPos(layout.media.pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(layout.media.size, ImGuiCond_Always);
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse;
    ImGui::Begin("Media", nullptr, flags);
    if (ImGui::BeginTabBar("MediaTabs")) {
        if (ImGui::BeginTabItem("Project")) {
            ImGui::InputText("Path", shellState->importMediaPath.data(), shellState->importMediaPath.size());
            ImGui::InputText("Label", shellState->importMediaLabel.data(), shellState->importMediaLabel.size());
            ImGui::InputText("Kind", shellState->importMediaKind.data(), shellState->importMediaKind.size());
            if (ImGui::Button("Import")) {
                applyCommand(shellState, jcut::ImportMediaCommand{
                    shellState->importMediaPath.data(),
                    shellState->importMediaLabel.data(),
                    shellState->importMediaKind.data()});
            }
            ImGui::Separator();
            const int trackId = selectedTrackId(snapshot);
            for (const jcut::EditorMediaItem& item : snapshot.mediaItems) {
                const bool selected = shellState->mediaSelectedPath == item.id;
                if (ImGui::Selectable(item.label.c_str(), selected)) {
                    shellState->mediaSelectedPath = item.id;
                }
                if (ImGui::IsItemHovered()) {
                    shellState->mediaHoveredPath = item.id;
                }
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    if (trackId > 0) {
                        applyCommand(shellState, jcut::InsertClipFromMediaCommand{
                            item.id,
                            trackId,
                            snapshot.transport.currentFrame,
                            90});
                    } else {
                        shellState->statusMessage = "select a track before inserting media";
                    }
                }
                ImGui::SameLine();
                ImGui::TextDisabled("(%s)", item.kind.c_str());
            }
            if (!shellState->mediaSelectedPath.empty()) {
                ImGui::Separator();
                ImGui::TextWrapped("%s", shellState->mediaSelectedPath.c_str());
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Files")) {
            if (shellState->mediaRootPath[0] == '\0') {
                std::snprintf(shellState->mediaRootPath.data(),
                              shellState->mediaRootPath.size(),
                              "%s",
                              shellState->projectRootPath.empty()
                                  ? fs::current_path().string().c_str()
                                  : shellState->projectRootPath.c_str());
            }
            ImGui::InputText("Root", shellState->mediaRootPath.data(), shellState->mediaRootPath.size());
            ImGui::InputText("Filter", shellState->mediaBrowserFilter.data(), shellState->mediaBrowserFilter.size());
            if (ImGui::Button("Use Project Root")) {
                std::snprintf(shellState->mediaRootPath.data(),
                              shellState->mediaRootPath.size(),
                              "%s",
                              shellState->projectRootPath.c_str());
                shellState->mediaGalleryPath.clear();
            }
            ImGui::SameLine();
            if (ImGui::Button("Up")) {
                fs::path current = shellState->mediaGalleryPath.empty()
                    ? fs::path(shellState->mediaRootPath.data())
                    : fs::path(shellState->mediaGalleryPath);
                if (current.has_parent_path()) {
                    const std::string parent = pathString(current.parent_path());
                    std::snprintf(shellState->mediaRootPath.data(),
                                  shellState->mediaRootPath.size(),
                                  "%s",
                                  parent.c_str());
                    shellState->mediaGalleryPath.clear();
                }
            }
            const fs::path activeRoot = shellState->mediaGalleryPath.empty()
                ? fs::path(shellState->mediaRootPath.data())
                : fs::path(shellState->mediaGalleryPath);
            ImGui::Separator();
            ImGui::TextWrapped("%s", pathString(activeRoot).c_str());
            const float browserHeight = std::max(160.0f, ImGui::GetContentRegionAvail().y - 92.0f);
            if (ImGui::BeginChild("FileSystemBrowser", ImVec2(0.0f, browserHeight), true)) {
                for (const fs::directory_entry& entry :
                     sortedDirectoryEntries(activeRoot, shellState->mediaBrowserFilter.data())) {
                    std::error_code ec;
                    const bool isDir = entry.is_directory(ec);
                    const fs::path entryPath = entry.path();
                    const bool selectable = isDir || isMediaFilePath(entryPath);
                    std::string label = isDir
                        ? "[dir] " + entryPath.filename().string()
                        : "[" + mediaKindForPath(entryPath) + "] " + entryPath.filename().string();
                    if (!selectable) {
                        ImGui::BeginDisabled();
                    }
                    const bool selected = shellState->mediaSelectedPath == pathString(entryPath);
                    if (ImGui::Selectable(label.c_str(), selected)) {
                        shellState->mediaSelectedPath = pathString(entryPath);
                    }
                    if (ImGui::IsItemHovered()) {
                        shellState->mediaHoveredPath = pathString(entryPath);
                    }
                    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        if (isDir) {
                            shellState->mediaGalleryPath = pathString(entryPath);
                        } else if (isMediaFilePath(entryPath)) {
                            importFilesystemMedia(shellState, snapshot, entryPath, true);
                        }
                    }
                    if (!selectable) {
                        ImGui::EndDisabled();
                    }
                }
            }
            ImGui::EndChild();
            const std::string previewPath = !shellState->mediaHoveredPath.empty()
                ? shellState->mediaHoveredPath
                : shellState->mediaSelectedPath;
            if (!previewPath.empty()) {
                ImGui::Separator();
                ImGui::TextWrapped("%s", previewPath.c_str());
                if (isMediaFilePath(previewPath) && ImGui::Button("Import Selected")) {
                    importFilesystemMedia(shellState, snapshot, previewPath, false);
                }
                ImGui::SameLine();
                if (isMediaFilePath(previewPath) && ImGui::Button("Insert Selected")) {
                    importFilesystemMedia(shellState, snapshot, previewPath, true);
                }
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void drawPreviewPanel(ShellState* shellState, const jcut::EditorDocumentCore& snapshot)
{
    const ShellLayout layout = computeShellLayout();
    ImGui::SetNextWindowPos(layout.preview.pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(layout.preview.size, ImGuiCond_Always);
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse;
    ImGui::Begin("Preview", nullptr, flags);
    ImVec2 avail = ImGui::GetContentRegionAvail();
    const float controlsHeight = 96.0f;
    const float canvasHeight = std::max(180.0f, avail.y - controlsHeight);
    const ImVec2 canvasSize(avail.x, canvasHeight);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    drawList->AddRectFilled(canvasPos,
                            ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
                            IM_COL32(22, 24, 28, 255),
                            6.0f);
    drawList->AddRect(canvasPos,
                      ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
                      IM_COL32(70, 78, 86, 255),
                      6.0f);

    const float targetAspect = snapshot.exportRequest.outputSize.height > 0
        ? static_cast<float>(snapshot.exportRequest.outputSize.width) /
            static_cast<float>(snapshot.exportRequest.outputSize.height)
        : (9.0f / 16.0f);
    const float paddedWidth = std::max(120.0f, canvasSize.x - 32.0f);
    const float paddedHeight = std::max(120.0f, canvasSize.y - 32.0f);
    float fittedFrameWidth = paddedWidth;
    float fittedFrameHeight = fittedFrameWidth / std::max(0.1f, targetAspect);
    if (fittedFrameHeight > paddedHeight) {
        fittedFrameHeight = paddedHeight;
        fittedFrameWidth = fittedFrameHeight * targetAspect;
    }
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 canvasMax(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y);
    const bool mouseInsideCanvas = ImGui::IsMouseHoveringRect(canvasPos, canvasMax);
    float zoom = snapshot.transport.previewZoom;
    const float oldZoom = zoom;
    if (mouseInsideCanvas && std::abs(io.MouseWheel) > 0.001f) {
        const float nextZoom = std::clamp(
            zoom * std::pow(1.12f, io.MouseWheel),
            0.5f,
            3.0f);
        if (std::abs(nextZoom - zoom) > 0.001f) {
            const ImVec2 canvasCenter(
                canvasPos.x + canvasSize.x * 0.5f,
                canvasPos.y + canvasSize.y * 0.5f);
            const ImVec2 mouseFromContentCenter(
                io.MousePos.x - (canvasCenter.x + shellState->previewPanX),
                io.MousePos.y - (canvasCenter.y + shellState->previewPanY));
            const float scale = nextZoom / std::max(0.001f, zoom);
            shellState->previewPanX -= mouseFromContentCenter.x * (scale - 1.0f);
            shellState->previewPanY -= mouseFromContentCenter.y * (scale - 1.0f);
            zoom = nextZoom;
            applyCommand(shellState, jcut::SetPreviewZoomCommand{zoom});
        }
    }
    if (mouseInsideCanvas &&
        (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f) ||
         ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f) ||
         ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f))) {
        shellState->previewPanX += io.MouseDelta.x;
        shellState->previewPanY += io.MouseDelta.y;
    }
    if (mouseInsideCanvas && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        shellState->previewPanX = 0.0f;
        shellState->previewPanY = 0.0f;
        if (std::abs(zoom - 1.0f) > 0.001f) {
            zoom = 1.0f;
            applyCommand(shellState, jcut::SetPreviewZoomCommand{zoom});
        }
    }
    if (std::abs(zoom - oldZoom) < 0.001f && zoom <= 1.001f) {
        shellState->previewPanX *= 0.85f;
        shellState->previewPanY *= 0.85f;
        if (std::abs(shellState->previewPanX) < 0.25f) shellState->previewPanX = 0.0f;
        if (std::abs(shellState->previewPanY) < 0.25f) shellState->previewPanY = 0.0f;
    }
    const float frameWidth = fittedFrameWidth * zoom;
    const float frameHeight = fittedFrameHeight * zoom;
    const float maxPanX = std::max(0.0f, (frameWidth - fittedFrameWidth) * 0.5f + 48.0f);
    const float maxPanY = std::max(0.0f, (frameHeight - fittedFrameHeight) * 0.5f + 48.0f);
    shellState->previewPanX = std::clamp(shellState->previewPanX, -maxPanX, maxPanX);
    shellState->previewPanY = std::clamp(shellState->previewPanY, -maxPanY, maxPanY);

    const ImVec2 frameMin(
        canvasPos.x + (canvasSize.x - frameWidth) * 0.5f + shellState->previewPanX,
        canvasPos.y + (canvasSize.y - frameHeight) * 0.5f + shellState->previewPanY);
    const ImVec2 frameMax(frameMin.x + frameWidth, frameMin.y + frameHeight);
    drawList->PushClipRect(canvasPos, canvasMax, true);
    drawList->AddRectFilled(frameMin, frameMax, IM_COL32(42, 46, 54, 255), 4.0f);
    if (shellState->previewTextureId != 0) {
        drawList->AddImage(shellState->previewTextureId,
                           frameMin,
                           frameMax,
                           ImVec2(0.0f, 0.0f),
                           ImVec2(1.0f, 1.0f));
    }
    drawList->AddRect(frameMin, frameMax, IM_COL32(160, 110, 56, 255), 4.0f, 0, 2.0f);
    const float inset = std::clamp(0.12f / std::max(0.5f, zoom), 0.04f, 0.18f);
    const ImVec2 safeMin(frameMin.x + frameWidth * inset, frameMin.y + frameHeight * 0.08f);
    const ImVec2 safeMax(frameMax.x - frameWidth * inset, frameMax.y - frameHeight * 0.08f);
    drawList->AddRect(safeMin, safeMax, IM_COL32(236, 160, 74, 255), 4.0f, 0, 2.0f);
    drawList->AddText(ImVec2(frameMin.x + 14.0f, frameMin.y + 12.0f),
                      IM_COL32(242, 242, 242, 255),
                      "Program");
    std::string previewDetail = "Frame " + std::to_string(snapshot.transport.currentFrame);
    {
        std::lock_guard<std::mutex> lock(shellState->previewMutex);
        if (!shellState->previewResult.message.empty()) {
            previewDetail += " | " + shellState->previewResult.message;
        }
    }
    drawList->AddText(ImVec2(frameMin.x + 14.0f, frameMin.y + 34.0f),
                      IM_COL32(180, 188, 198, 255),
                      previewDetail.c_str());
    drawList->PopClipRect();

    ImGui::Dummy(canvasSize);
    ImGui::Separator();
    if (ImGui::Button(snapshot.transport.playbackActive ? "Pause" : "Play")) {
        applyCommand(shellState, jcut::TogglePlaybackCommand{});
    }
    ImGui::SameLine();
    if (ImGui::ArrowButton("StepBack", ImGuiDir_Left)) {
        applyCommand(shellState, jcut::StepFrameCommand{-1});
    }
    ImGui::SameLine();
    if (ImGui::ArrowButton("StepForward", ImGuiDir_Right)) {
        applyCommand(shellState, jcut::StepFrameCommand{1});
    }
    float speed = snapshot.transport.playbackSpeed;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::SliderFloat("Speed", &speed, 0.25f, 2.0f, "%.2fx")) {
        applyCommand(shellState, jcut::SetPlaybackSpeedCommand{speed});
    }
    float zoomValue = snapshot.transport.previewZoom;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::SliderFloat("Zoom", &zoomValue, 0.5f, 3.0f, "%.2fx")) {
        applyCommand(shellState, jcut::SetPreviewZoomCommand{zoomValue});
    }
    int currentFrame = snapshot.transport.currentFrame;
    if (ImGui::InputInt("Frame", &currentFrame)) {
        applyCommand(shellState, jcut::SeekToFrameCommand{currentFrame});
    }
    ImGui::End();
}

void drawTimelinePanel(ShellState* shellState, const jcut::EditorDocumentCore& snapshot)
{
    const ShellLayout layout = computeShellLayout();
    ImGui::SetNextWindowPos(layout.timeline.pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(layout.timeline.size, ImGuiCond_Always);
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse;
    ImGui::Begin("Timeline", nullptr, flags);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), IM_COL32(18, 20, 24, 255), 6.0f);

    constexpr std::array<ImU32, 4> trackColors = {
        IM_COL32(54, 110, 156, 255),
        IM_COL32(96, 132, 66, 255),
        IM_COL32(168, 106, 38, 255),
        IM_COL32(132, 82, 140, 255)
    };

    int hoveredClipId = 0;
    int hoveredTrackId = 0;
    TimelineDragMode hoveredMode = TimelineDragMode::None;
    const ImVec2 mousePos = ImGui::GetIO().MousePos;

    for (std::size_t i = 0; i < snapshot.tracks.size(); ++i) {
        const jcut::EditorTrack& track = snapshot.tracks[i];
        const float y = origin.y + kTimelineTopPadding + static_cast<float>(i) * kTimelineRowHeight;
        drawList->AddText(ImVec2(origin.x + 10.0f, y + 6.0f),
                          track.selected ? IM_COL32(255, 214, 140, 255) : IM_COL32(224, 228, 232, 255),
                          track.label.c_str());
        drawList->AddRectFilled(ImVec2(origin.x + kTimelineLabelWidth, y),
                                ImVec2(origin.x + avail.x - 12.0f, y + kTimelineClipHeight),
                                IM_COL32(34, 38, 44, 255),
                                4.0f);
        for (const jcut::EditorClip& clip : snapshot.clips) {
            if (clip.trackId != track.id) {
                continue;
            }
            const float clipStart = origin.x + kTimelineLabelWidth + kTimelineTrackPadding +
                static_cast<float>(clip.startFrame) * kTimelinePixelsPerFrame;
            const float clipWidth = std::max(40.0f, static_cast<float>(clip.durationFrames) * kTimelinePixelsPerFrame);
            const ImVec2 clipMin(clipStart, y + 2.0f);
            const ImVec2 clipMax(clipStart + clipWidth, y + kTimelineClipHeight - 2.0f);
            const ImU32 color = clip.selected ? IM_COL32(255, 196, 86, 255)
                                              : trackColors[i % trackColors.size()];
            drawList->AddRectFilled(clipMin,
                                    clipMax,
                                    color,
                                    4.0f);
            drawList->AddText(ImVec2(clipStart + 8.0f, y + 6.0f), IM_COL32(245, 245, 245, 255), clip.label.c_str());
            if (clip.selected) {
                drawList->AddRectFilled(clipMin,
                                        ImVec2(clipMin.x + kTimelineHandleWidth, clipMax.y),
                                        IM_COL32(255, 228, 160, 180),
                                        3.0f);
                drawList->AddRectFilled(ImVec2(clipMax.x - kTimelineHandleWidth, clipMin.y),
                                        clipMax,
                                        IM_COL32(255, 228, 160, 180),
                                        3.0f);
            }

            if (ImGui::IsMouseHoveringRect(clipMin, clipMax)) {
                hoveredClipId = clip.id;
                hoveredTrackId = track.id;
                hoveredMode = TimelineDragMode::MoveClip;
                if (clip.selected && mousePos.x <= clipMin.x + kTimelineHandleWidth) {
                    hoveredMode = TimelineDragMode::TrimClipStart;
                } else if (clip.selected && mousePos.x >= clipMax.x - kTimelineHandleWidth) {
                    hoveredMode = TimelineDragMode::TrimClipEnd;
                }
            }
        }
    }

    const float playheadX = origin.x + kTimelineLabelWidth + kTimelineTrackPadding +
        static_cast<float>(snapshot.transport.currentFrame) * kTimelinePixelsPerFrame;
    drawList->AddLine(ImVec2(playheadX, origin.y + 6.0f),
                      ImVec2(playheadX, origin.y + avail.y - 6.0f),
                      IM_COL32(255, 196, 86, 255),
                      2.0f);

    const bool mouseInsideCanvas = ImGui::IsMouseHoveringRect(origin, ImVec2(origin.x + avail.x, origin.y + avail.y));
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && mouseInsideCanvas) {
        if (hoveredClipId != 0) {
            applyCommand(shellState, jcut::SelectTrackCommand{hoveredTrackId});
            applyCommand(shellState, jcut::SelectClipCommand{hoveredClipId});
            shellState->timelineDragMode = hoveredMode;
            shellState->timelineDragClipId = hoveredClipId;
            shellState->timelineDragTrackId = hoveredTrackId;
            shellState->timelineDragMouseX = mousePos.x;
            shellState->timelineDragMouseY = mousePos.y;
            for (std::size_t i = 0; i < snapshot.tracks.size(); ++i) {
                if (snapshot.tracks[i].id == hoveredTrackId) {
                    shellState->timelineDragTrackIndex = static_cast<int>(i);
                    break;
                }
            }
            for (const jcut::EditorClip& clip : snapshot.clips) {
                if (clip.id == hoveredClipId) {
                    shellState->timelineDragStartFrame = clip.startFrame;
                    shellState->timelineDragDurationFrames = clip.durationFrames;
                    break;
                }
            }
        } else {
            const int trackIndex = trackIndexFromTimelineY(snapshot, origin.y, mousePos.y);
            if (trackIndex >= 0) {
                applyCommand(shellState, jcut::SelectTrackCommand{snapshot.tracks[trackIndex].id});
            }
            applyCommand(shellState, jcut::SeekToFrameCommand{frameFromTimelineX(origin.x, mousePos.x)});
            shellState->timelineDragMode = TimelineDragMode::Seek;
        }
    }

    if (shellState->timelineDragMode != TimelineDragMode::None && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (shellState->timelineDragMode == TimelineDragMode::Seek) {
            applyCommand(shellState, jcut::SeekToFrameCommand{frameFromTimelineX(origin.x, mousePos.x)});
        } else {
            const int deltaFrames = static_cast<int>(std::lround(
                (mousePos.x - shellState->timelineDragMouseX) / kTimelinePixelsPerFrame));
            if (shellState->timelineDragMode == TimelineDragMode::MoveClip) {
                int targetTrackId = shellState->timelineDragTrackId;
                const int hoveredTrackIndex = trackIndexFromTimelineY(snapshot, origin.y, mousePos.y);
                if (hoveredTrackIndex >= 0) {
                    targetTrackId = snapshot.tracks[hoveredTrackIndex].id;
                }
                applyCommand(shellState, jcut::MoveClipCommand{
                    shellState->timelineDragClipId,
                    targetTrackId,
                    shellState->timelineDragStartFrame + deltaFrames});
            } else if (shellState->timelineDragMode == TimelineDragMode::TrimClipStart) {
                applyCommand(shellState, jcut::TrimClipStartCommand{
                    shellState->timelineDragClipId,
                    shellState->timelineDragStartFrame + deltaFrames});
            } else if (shellState->timelineDragMode == TimelineDragMode::TrimClipEnd) {
                applyCommand(shellState, jcut::TrimClipEndCommand{
                    shellState->timelineDragClipId,
                    shellState->timelineDragStartFrame + shellState->timelineDragDurationFrames + deltaFrames});
            }
        }
    }
    if (shellState->timelineDragMode != TimelineDragMode::None && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        clearTimelineDrag(shellState);
    }

    ImGui::Dummy(avail);

    ImGui::Separator();
    for (const jcut::EditorTrack& track : snapshot.tracks) {
        if (ImGui::Selectable(track.label.c_str(), track.selected)) {
            applyCommand(shellState, jcut::SelectTrackCommand{track.id});
        }
    }
    for (const jcut::EditorClip& clip : snapshot.clips) {
        if (ImGui::Selectable(clip.label.c_str(), clip.selected)) {
            applyCommand(shellState, jcut::SelectClipCommand{clip.id});
        }
    }
    ImGui::End();
}

void drawInspectorHeading(const char* title,
                          const jcut::EditorDocumentCore& snapshot,
                          const jcut::EditorClip* clip)
{
    ImGui::TextUnformatted(title);
    ImGui::Separator();
    if (clip) {
        ImGui::Text("Clip %d | Track %d", clip->id, clip->trackId);
        ImGui::TextWrapped("%s", clip->sourcePath.empty() ? clip->label.c_str() : clip->sourcePath.c_str());
    } else {
        ImGui::Text("Project | %zu clips | %zu tracks | %zu media",
                    snapshot.clips.size(),
                    snapshot.tracks.size(),
                    snapshot.mediaItems.size());
    }
}

void drawReadOnlyTableRow(const char* key, const std::string& value)
{
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(key);
    ImGui::TableNextColumn();
    ImGui::TextWrapped("%s", value.c_str());
}

void drawClipSummaryTable(const jcut::EditorDocumentCore& snapshot,
                          const jcut::EditorClip* selected)
{
    if (!ImGui::BeginTable("ClipSummary", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        return;
    }
    drawReadOnlyTableRow("Project", snapshot.projectName);
    drawReadOnlyTableRow("Current Frame", std::to_string(snapshot.transport.currentFrame));
    drawReadOnlyTableRow("Playback", snapshot.transport.playbackActive ? "Playing" : "Stopped");
    drawReadOnlyTableRow("Speed", std::to_string(snapshot.transport.playbackSpeed));
    if (selected) {
        drawReadOnlyTableRow("Selected Clip", selected->label);
        drawReadOnlyTableRow("Source", selected->sourcePath);
        drawReadOnlyTableRow("Start", std::to_string(selected->startFrame));
        drawReadOnlyTableRow("Duration", std::to_string(selected->durationFrames));
    }
    ImGui::EndTable();
}

void drawClipsTable(ShellState* shellState, const jcut::EditorDocumentCore& snapshot)
{
    if (!ImGui::BeginTable("ClipsTable", 5,
                           ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
        return;
    }
    ImGui::TableSetupColumn("Name");
    ImGui::TableSetupColumn("Track");
    ImGui::TableSetupColumn("Start");
    ImGui::TableSetupColumn("Duration");
    ImGui::TableSetupColumn("File");
    ImGui::TableHeadersRow();
    for (const jcut::EditorClip& clip : snapshot.clips) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (ImGui::Selectable(clip.label.c_str(), clip.selected, ImGuiSelectableFlags_SpanAllColumns)) {
            applyCommand(shellState, jcut::SelectClipCommand{clip.id});
        }
        ImGui::TableNextColumn();
        ImGui::Text("%d", clip.trackId);
        ImGui::TableNextColumn();
        ImGui::Text("%d", clip.startFrame);
        ImGui::TableNextColumn();
        ImGui::Text("%d", clip.durationFrames);
        ImGui::TableNextColumn();
        ImGui::TextWrapped("%s", clip.sourcePath.c_str());
    }
    ImGui::EndTable();
}

void drawTracksTable(ShellState* shellState, const jcut::EditorDocumentCore& snapshot)
{
    if (!ImGui::BeginTable("TracksTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        return;
    }
    ImGui::TableSetupColumn("Track");
    ImGui::TableSetupColumn("Visual");
    ImGui::TableSetupColumn("Audio");
    ImGui::TableHeadersRow();
    for (const jcut::EditorTrack& track : snapshot.tracks) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (ImGui::Selectable(track.label.c_str(), track.selected, ImGuiSelectableFlags_SpanAllColumns)) {
            applyCommand(shellState, jcut::SelectTrackCommand{track.id});
        }
        ImGui::TableNextColumn();
        ImGui::TextUnformatted("On");
        ImGui::TableNextColumn();
        ImGui::TextUnformatted("On");
    }
    ImGui::EndTable();
}

void drawInspectorPanel(ShellState* shellState, const jcut::EditorDocumentCore& snapshot)
{
    const ShellLayout layout = computeShellLayout();
    ImGui::SetNextWindowPos(layout.inspector.pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(layout.inspector.size, ImGuiCond_Always);
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse;
    ImGui::Begin("Inspector", nullptr, flags);
    const jcut::EditorClip* currentClip = selectedClip(snapshot);
    const jcut::EditorTrack* currentTrack = selectedTrack(snapshot);
    if (ImGui::BeginTabBar("InspectorTabs")) {
        if (ImGui::BeginTabItem("Grade")) {
            drawInspectorHeading("Grade", snapshot, currentClip);
            float saturation = 1.0f;
            float brightness = 0.0f;
            float contrast = 1.0f;
            ImGui::SliderFloat("Saturation", &saturation, 0.0f, 2.0f, "%.2f");
            ImGui::SliderFloat("Brightness", &brightness, -1.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("Contrast", &contrast, 0.0f, 2.0f, "%.2f");
            if (ImGui::BeginTabBar("GradeChannels")) {
                for (const char* channel : {"Red", "Green", "Blue", "Brightness"}) {
                    if (ImGui::BeginTabItem(channel)) {
                        ImGui::PlotLines("Histogram", std::array<float, 16>{
                            0.12f, 0.18f, 0.22f, 0.28f, 0.44f, 0.62f, 0.74f, 0.70f,
                            0.58f, 0.42f, 0.35f, 0.28f, 0.20f, 0.15f, 0.10f, 0.08f
                        }.data(), 16, 0, nullptr, 0.0f, 1.0f, ImVec2(-1.0f, 72.0f));
                        ImGui::EndTabItem();
                    }
                }
                ImGui::EndTabBar();
            }
            bool gradePreview = snapshot.panels.showScopes;
            if (ImGui::Checkbox("Preview", &gradePreview)) {
                applyCommand(shellState, jcut::SetScopesVisibleCommand{gradePreview});
            }
            ImGui::Button("Key At Playhead");
            ImGui::SameLine();
            ImGui::Button("Auto Oppose");
            if (ImGui::BeginTable("GradeKeys", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Frame");
                ImGui::TableSetupColumn("Bright");
                ImGui::TableSetupColumn("Contrast");
                ImGui::TableSetupColumn("Sat");
                ImGui::TableSetupColumn("Interp");
                ImGui::TableHeadersRow();
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Opacity")) {
            drawInspectorHeading("Opacity", snapshot, currentClip);
            float opacity = 1.0f;
            ImGui::SliderFloat("Opacity", &opacity, 0.0f, 1.0f, "%.2f");
            ImGui::Button("Key At Playhead");
            ImGui::SameLine();
            ImGui::Button("Fade In From Playhead");
            ImGui::SameLine();
            ImGui::Button("Fade Out From Playhead");
            if (ImGui::BeginTable("OpacityKeys", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Frame");
                ImGui::TableSetupColumn("Opacity");
                ImGui::TableSetupColumn("Interp");
                ImGui::TableHeadersRow();
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Effects")) {
            drawInspectorHeading("Effects", snapshot, currentClip);
            bool feather = false;
            float radius = 8.0f;
            float gamma = 1.0f;
            ImGui::Checkbox("Enable Mask Feathering", &feather);
            ImGui::SliderFloat("Radius", &radius, 0.0f, 64.0f, "%.1f");
            ImGui::SliderFloat("Curve Gamma", &gamma, 0.1f, 4.0f, "%.2f");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Corrections")) {
            drawInspectorHeading("Corrections", snapshot, currentClip);
            bool enabled = false;
            bool drawPolygons = false;
            ImGui::Checkbox("Enable Corrections", &enabled);
            ImGui::Checkbox("Draw Polygons In Preview", &drawPolygons);
            if (ImGui::BeginTable("PolygonRanges", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("On");
                ImGui::TableSetupColumn("Start");
                ImGui::TableSetupColumn("End");
                ImGui::TableSetupColumn("Points");
                ImGui::TableHeadersRow();
                ImGui::EndTable();
            }
            ImGui::Button("Draw Polygon");
            ImGui::SameLine();
            ImGui::Button("Close Polygon");
            ImGui::SameLine();
            ImGui::Button("Clear All Polygons");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Titles")) {
            drawInspectorHeading("Titles", snapshot, currentClip);
            static char titleText[512] = {};
            ImGui::InputTextMultiline("Title Text", titleText, sizeof(titleText), ImVec2(-1.0f, 90.0f));
            float x = 0.5f;
            float y = 0.5f;
            int fontSize = 42;
            ImGui::SliderFloat("X", &x, 0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("Y", &y, 0.0f, 1.0f, "%.2f");
            ImGui::InputInt("Font Size", &fontSize);
            ImGui::Button("Add Title At Playhead");
            ImGui::SameLine();
            ImGui::Button("Remove Selected");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Sync")) {
            drawInspectorHeading("Sync", snapshot, currentClip);
            ImGui::Button("Clear All Sync Points");
            if (ImGui::BeginTable("SyncPoints", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Clip");
                ImGui::TableSetupColumn("Frame");
                ImGui::TableSetupColumn("Count");
                ImGui::TableSetupColumn("Action");
                ImGui::TableHeadersRow();
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Transform")) {
            drawInspectorHeading("Transform", snapshot, currentClip);
            float tx = 0.0f;
            float ty = 0.0f;
            float rotation = 0.0f;
            float scaleX = 1.0f;
            float scaleY = 1.0f;
            ImGui::SliderFloat("Translate X", &tx, -1.0f, 1.0f, "%.3f");
            ImGui::SliderFloat("Translate Y", &ty, -1.0f, 1.0f, "%.3f");
            ImGui::SliderFloat("Rotation", &rotation, -180.0f, 180.0f, "%.1f");
            ImGui::SliderFloat("Scale X", &scaleX, 0.1f, 4.0f, "%.2f");
            ImGui::SliderFloat("Scale Y", &scaleY, 0.1f, 4.0f, "%.2f");
            ImGui::Button("Add Keyframe");
            ImGui::SameLine();
            ImGui::Button("Flip Horizontal");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Transcript")) {
            drawInspectorHeading("Transcript", snapshot, currentClip);
            bool transcript = snapshot.panels.showTranscript;
            if (ImGui::Checkbox("Enable Overlay", &transcript)) {
                applyCommand(shellState, jcut::SetTranscriptVisibleCommand{transcript});
            }
            int maxLines = 2;
            int maxChars = 42;
            float width = 0.82f;
            float height = 0.24f;
            ImGui::InputInt("Max Lines", &maxLines);
            ImGui::InputInt("Max Chars", &maxChars);
            ImGui::SliderFloat("Width", &width, 0.1f, 1.0f, "%.2f");
            ImGui::SliderFloat("Height", &height, 0.1f, 1.0f, "%.2f");
            if (ImGui::BeginTable("TranscriptRows", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Source Start");
                ImGui::TableSetupColumn("Source End");
                ImGui::TableSetupColumn("Speaker");
                ImGui::TableSetupColumn("Text");
                ImGui::TableSetupColumn("Edits");
                ImGui::TableHeadersRow();
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Speakers")) {
            drawInspectorHeading("Speakers", snapshot, currentClip);
            ImGui::Button("Mine Transcript (AI)");
            ImGui::SameLine();
            ImGui::Button("Find Organizations");
            ImGui::SameLine();
            ImGui::Button("Clean Assignments");
            bool continuity = false;
            bool detections = false;
            ImGui::Checkbox("Show Continuity Tracks in Preview", &continuity);
            ImGui::Checkbox("Show Raw Detections in Preview", &detections);
            if (ImGui::BeginTable("SpeakersRoster", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Avatar");
                ImGui::TableSetupColumn("Speaker");
                ImGui::TableSetupColumn("X");
                ImGui::TableSetupColumn("Y");
                ImGui::TableSetupColumn("Assigned Tracks");
                ImGui::TableHeadersRow();
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Properties")) {
            drawInspectorHeading("Properties", snapshot, currentClip);
            drawClipSummaryTable(snapshot, currentClip);
            if (currentTrack) {
                ImGui::Separator();
                ImGui::Text("Track %d", currentTrack->id);
                ImGui::TextWrapped("%s", currentTrack->label.c_str());
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Clips")) {
            drawInspectorHeading("Clips", snapshot, currentClip);
            drawClipsTable(shellState, snapshot);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("History")) {
            drawInspectorHeading("History", snapshot, currentClip);
            if (ImGui::BeginTable("HistoryTable", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Index");
                ImGui::TableSetupColumn("Summary");
                ImGui::TableHeadersRow();
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Tracks")) {
            drawInspectorHeading("Tracks", snapshot, currentClip);
            drawTracksTable(shellState, snapshot);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Preview")) {
            drawInspectorHeading("Preview", snapshot, currentClip);
            bool scopes = snapshot.panels.showScopes;
            if (ImGui::Checkbox("Show Scopes", &scopes)) {
                applyCommand(shellState, jcut::SetScopesVisibleCommand{scopes});
            }
            float zoom = snapshot.transport.previewZoom;
            if (ImGui::SliderFloat("Zoom", &zoom, 0.5f, 3.0f, "%.2fx")) {
                applyCommand(shellState, jcut::SetPreviewZoomCommand{zoom});
            }
            if (ImGui::Button("Reset")) {
                shellState->previewPanX = 0.0f;
                shellState->previewPanY = 0.0f;
                applyCommand(shellState, jcut::SetPreviewZoomCommand{1.0f});
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Audio")) {
            drawInspectorHeading("Audio", snapshot, currentClip);
            bool waveform = snapshot.panels.showWaveform;
            if (ImGui::Checkbox("Waveform", &waveform)) {
                applyCommand(shellState, jcut::SetWaveformVisibleCommand{waveform});
            }
            if (ImGui::BeginTable("AudioStatus", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                drawReadOnlyTableRow("Initialized", shellState->audioInitialized ? "yes" : "no");
                drawReadOnlyTableRow("Timeline", shellState->audioTimelineConfigured ? "configured" : "pending");
                drawReadOnlyTableRow("Playback", shellState->audioPlaybackActive ? "active" : "idle");
                drawReadOnlyTableRow("Status", shellState->audioStatusMessage);
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("AI Assist")) {
            drawInspectorHeading("AI Assist", snapshot, currentClip);
            static char chatInput[1024] = {};
            const char* models[] = {"default", "fast", "high quality"};
            static int modelIndex = 0;
            ImGui::Combo("Model", &modelIndex, models, IM_ARRAYSIZE(models));
            ImGui::Button("Transcribe (AI)");
            ImGui::SameLine();
            ImGui::Button("Mine Transcript (AI)");
            ImGui::InputTextMultiline("Chat", chatInput, sizeof(chatInput), ImVec2(-1.0f, 120.0f));
            ImGui::Button("Send");
            ImGui::SameLine();
            ImGui::Button("Clear");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Access")) {
            drawInspectorHeading("Subscriptions & Purchases", snapshot, currentClip);
            if (ImGui::BeginTable("AccessTable", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Type");
                ImGui::TableSetupColumn("Item");
                ImGui::TableSetupColumn("Status");
                ImGui::TableSetupColumn("Period");
                ImGui::TableSetupColumn("Source");
                ImGui::TableHeadersRow();
                ImGui::EndTable();
            }
            ImGui::Button("Refresh");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Project")) {
            char projectName[256];
            std::snprintf(projectName, sizeof(projectName), "%s", snapshot.projectName.c_str());
            if (ImGui::InputText("Name", projectName, sizeof(projectName),
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
                applyCommand(shellState, jcut::SetProjectNameCommand{projectName});
            }
            if (ImGui::Button("Add Track")) {
                applyCommand(shellState, jcut::AddTrackCommand{});
            }
            ImGui::SameLine();
            const jcut::EditorTrack* selectedTrack = nullptr;
            for (const jcut::EditorTrack& track : snapshot.tracks) {
                if (track.selected) {
                    selectedTrack = &track;
                    break;
                }
            }
            if (ImGui::Button("Add Clip") && selectedTrack) {
                applyCommand(shellState, jcut::AddClipCommand{
                    selectedTrack->id,
                    {},
                    snapshot.transport.currentFrame,
                    90,
                    {},
                    {}});
            }
            ImGui::SameLine();
            if (ImGui::Button("Delete Track") && selectedTrack) {
                applyCommand(shellState, jcut::DeleteTrackCommand{selectedTrack->id});
            }
            ImGui::Text("Tracks %zu", snapshot.tracks.size());
            ImGui::Text("Clips %zu", snapshot.clips.size());
            ImGui::Text("Media %zu", snapshot.mediaItems.size());
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Clip")) {
            const jcut::EditorClip* selectedClip = nullptr;
            for (const jcut::EditorClip& clip : snapshot.clips) {
                if (clip.selected) {
                    selectedClip = &clip;
                    break;
                }
            }
            if (selectedClip) {
                char clipLabel[256];
                std::snprintf(clipLabel, sizeof(clipLabel), "%s", selectedClip->label.c_str());
                if (ImGui::InputText("Label", clipLabel, sizeof(clipLabel),
                                     ImGuiInputTextFlags_EnterReturnsTrue)) {
                    applyCommand(shellState, jcut::SetClipLabelCommand{selectedClip->id, clipLabel});
                }

                int trackId = selectedClip->trackId;
                if (ImGui::InputInt("Track", &trackId)) {
                    applyCommand(shellState, jcut::MoveClipCommand{
                        selectedClip->id, trackId, selectedClip->startFrame});
                }

                int startFrame = selectedClip->startFrame;
                if (ImGui::InputInt("Start", &startFrame)) {
                    applyCommand(shellState, jcut::MoveClipCommand{
                        selectedClip->id, selectedClip->trackId, startFrame});
                }

                int durationFrames = selectedClip->durationFrames;
                if (ImGui::InputInt("Duration", &durationFrames)) {
                    applyCommand(shellState, jcut::ResizeClipCommand{
                        selectedClip->id, durationFrames});
                }

                if (ImGui::Button("Delete Clip")) {
                    applyCommand(shellState, jcut::DeleteClipCommand{selectedClip->id});
                }
                ImGui::SameLine();
                if (ImGui::Button("Split At Playhead")) {
                    applyCommand(shellState, jcut::SplitClipCommand{
                        selectedClip->id,
                        snapshot.transport.currentFrame});
                }

                ImGui::Text("Clip %d", selectedClip->id);
                ImGui::TextUnformatted(selectedClip->sourcePath.empty()
                    ? "No source path"
                    : selectedClip->sourcePath.c_str());
            } else {
                ImGui::TextUnformatted("No clip selected");
            }
            bool waveform = snapshot.panels.showWaveform;
            if (ImGui::Checkbox("Waveform", &waveform)) {
                applyCommand(shellState, jcut::SetWaveformVisibleCommand{waveform});
            }
            bool transcript = snapshot.panels.showTranscript;
            if (ImGui::Checkbox("Transcript", &transcript)) {
                applyCommand(shellState, jcut::SetTranscriptVisibleCommand{transcript});
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Output")) {
            jcut::render::RenderProgressCore exportProgress;
            jcut::render::RenderResultCore exportResult;
            bool exportRunning = false;
            bool exportHasProgress = false;
            {
                std::lock_guard<std::mutex> lock(shellState->exportMutex);
                exportProgress = shellState->exportProgress;
                exportResult = shellState->exportResult;
                exportRunning = shellState->exportRunning;
                exportHasProgress = shellState->exportHasProgress;
            }
            int width = snapshot.exportRequest.outputSize.width;
            int height = snapshot.exportRequest.outputSize.height;
            float fps = static_cast<float>(snapshot.exportRequest.outputFps);
            const std::array<const char*, 3> formats = {"mp4", "mov", "mkv"};
            int formatIndex = 0;
            for (int i = 0; i < static_cast<int>(formats.size()); ++i) {
                if (snapshot.exportRequest.outputFormat == formats[i]) {
                    formatIndex = i;
                    break;
                }
            }
            if (ImGui::InputInt("Width", &width)) {
                applyCommand(shellState, jcut::SetExportSizeCommand{width, height});
            }
            if (ImGui::InputInt("Height", &height)) {
                applyCommand(shellState, jcut::SetExportSizeCommand{width, height});
            }
            if (ImGui::InputFloat("FPS", &fps, 0.5f, 2.0f, "%.2f")) {
                applyCommand(shellState, jcut::SetExportFpsCommand{fps});
            }
            if (ImGui::InputText("Output Path",
                                 shellState->exportOutputPath.data(),
                                 shellState->exportOutputPath.size(),
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
                applyCommand(shellState, jcut::SetExportOutputPathCommand{
                    shellState->exportOutputPath.data()});
            }
            if (ImGui::Combo("Format", &formatIndex, formats.data(), static_cast<int>(formats.size()))) {
                applyCommand(shellState, jcut::SetExportFormatCommand{formats[formatIndex]});
            }
            bool useProxies = snapshot.exportRequest.useProxyMedia;
            if (ImGui::Checkbox("Use Proxies", &useProxies)) {
                applyCommand(shellState, jcut::SetExportUseProxyMediaCommand{useProxies});
            }
            bool imageSequence = snapshot.exportRequest.createVideoFromImageSequence;
            if (ImGui::Checkbox("Image Sequence", &imageSequence)) {
                applyCommand(shellState, jcut::SetExportImageSequenceCommand{imageSequence});
            }
            if (imageSequence) {
                const std::array<const char*, 3> sequenceFormats = {"jpeg", "png", "webp"};
                int sequenceFormatIndex = 0;
                for (int i = 0; i < static_cast<int>(sequenceFormats.size()); ++i) {
                    if (snapshot.exportRequest.imageSequenceFormat == sequenceFormats[i]) {
                        sequenceFormatIndex = i;
                        break;
                    }
                }
                if (ImGui::Combo("Sequence Format",
                                 &sequenceFormatIndex,
                                 sequenceFormats.data(),
                                 static_cast<int>(sequenceFormats.size()))) {
                    applyCommand(shellState,
                                 jcut::SetExportImageSequenceFormatCommand{
                                     sequenceFormats[sequenceFormatIndex]});
                }
            }
            ImGui::Separator();
            if (exportRunning) {
                const float completion = exportProgress.totalFrames > 0
                    ? static_cast<float>(exportProgress.framesCompleted) /
                        static_cast<float>(exportProgress.totalFrames)
                    : 0.0f;
                ImGui::ProgressBar(completion, ImVec2(-1.0f, 0.0f));
                ImGui::Text("Frame %lld / %lld",
                            static_cast<long long>(exportProgress.framesCompleted),
                            static_cast<long long>(exportProgress.totalFrames));
                if (!exportProgress.encoderLabel.empty()) {
                    ImGui::TextUnformatted(exportProgress.encoderLabel.c_str());
                }
                if (ImGui::Button("Cancel Export")) {
                    cancelExportRender(shellState);
                    shellState->statusMessage = "export cancellation requested";
                }
            } else {
                if (ImGui::Button("Export")) {
                    if (requestExportRender(shellState)) {
                        shellState->statusMessage = "export started";
                    } else {
                        shellState->statusMessage = "export already running";
                    }
                }
                if (exportHasProgress || !exportResult.message.empty()) {
                    ImGui::Separator();
                    ImGui::Text("Frames %lld",
                                static_cast<long long>(exportResult.framesRendered));
                    if (!exportResult.encoderLabel.empty()) {
                        ImGui::TextUnformatted(exportResult.encoderLabel.c_str());
                    }
                    ImGui::TextUnformatted(exportResult.message.c_str());
                }
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Scopes")) {
            bool scopes = snapshot.panels.showScopes;
            if (ImGui::Checkbox("Visible", &scopes)) {
                applyCommand(shellState, jcut::SetScopesVisibleCommand{scopes});
            }
            const ImVec2 scopeAvail = ImGui::GetContentRegionAvail();
            const ImVec2 scopePos = ImGui::GetCursorScreenPos();
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            drawList->AddRectFilled(scopePos,
                                    ImVec2(scopePos.x + scopeAvail.x, scopePos.y + 160.0f),
                                    IM_COL32(16, 18, 22, 255),
                                    4.0f);
            drawList->AddRect(scopePos,
                              ImVec2(scopePos.x + scopeAvail.x, scopePos.y + 160.0f),
                              IM_COL32(80, 88, 96, 255),
                              4.0f);
            ImGui::Dummy(ImVec2(scopeAvail.x, 160.0f));
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Pipeline")) {
            drawInspectorHeading("Pipeline", snapshot, currentClip);
            const ImVec2 pipelinePreviewSize(-1.0f, 180.0f);
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const float width = ImGui::GetContentRegionAvail().x;
            drawList->AddRectFilled(pos,
                                    ImVec2(pos.x + width, pos.y + pipelinePreviewSize.y),
                                    IM_COL32(18, 20, 24, 255),
                                    4.0f);
            drawList->AddText(ImVec2(pos.x + 12.0f, pos.y + 12.0f),
                              IM_COL32(220, 226, 232, 255),
                              "Pipeline Preview");
            ImGui::Dummy(pipelinePreviewSize);
            if (ImGui::BeginTable("PipelineStages", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                drawReadOnlyTableRow("Decode", "hardware preferred");
                drawReadOnlyTableRow("Render", "Vulkan preview core");
                drawReadOnlyTableRow("Present", "raw X11 Vulkan swapchain");
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("System")) {
            drawInspectorHeading("System", snapshot, currentClip);
            const char* threadingModes[] = {
                "Auto (Recommended)",
                "Single Thread (Safest)",
                "Slice Threads (Balanced)",
                "Frame + Slice Threads (Fastest)"
            };
            static int threadingMode = 0;
            ImGui::Combo("H.264/H.265 CPU Threading", &threadingMode, threadingModes, IM_ARRAYSIZE(threadingModes));
            if (ImGui::BeginTable("SystemProfile", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                drawReadOnlyTableRow("Backend", "imgui");
                drawReadOnlyTableRow("Window", "x11/vulkan");
                drawReadOnlyTableRow("Media", std::to_string(snapshot.mediaItems.size()));
                drawReadOnlyTableRow("Clips", std::to_string(snapshot.clips.size()));
                drawReadOnlyTableRow("Tracks", std::to_string(snapshot.tracks.size()));
                drawReadOnlyTableRow("Audio", shellState->audioStatusMessage);
                ImGui::EndTable();
            }
            ImGui::Button("Run Decode Benchmark");
            ImGui::SameLine();
            ImGui::Button("Restart All Decoders");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Projects")) {
            drawInspectorHeading("Projects", snapshot, currentClip);
            ImGui::TextWrapped("%s", shellState->projectId.empty()
                ? snapshot.projectName.c_str()
                : shellState->projectId.c_str());
            ImGui::TextWrapped("%s", shellState->statePath.empty()
                ? shellState->documentPath.c_str()
                : shellState->statePath.c_str());
            if (ImGui::Button("Save As")) {
                saveCurrentDocument(shellState);
            }
            ImGui::SameLine();
            ImGui::Button("New");
            ImGui::SameLine();
            ImGui::Button("Rename");
            ImGui::Separator();
            ImGui::TextWrapped("%s", shellState->projectRootPath.c_str());
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Preferences")) {
            drawInspectorHeading("Preferences", snapshot, currentClip);
            float uiFontSize = shellState->uiFontSize;
            if (ImGui::SliderFloat("UI Font Size", &uiFontSize, kMinUiFontSize, kMaxUiFontSize, "%.0f")) {
                changeUiFontSize(shellState, uiFontSize - shellState->uiFontSize);
            }
            bool waveform = snapshot.panels.showWaveform;
            if (ImGui::Checkbox("Enable Audio Preview Mode", &waveform)) {
                applyCommand(shellState, jcut::SetWaveformVisibleCommand{waveform});
            }
            bool transcript = snapshot.panels.showTranscript;
            if (ImGui::Checkbox("Enable Transcript Overlay", &transcript)) {
                applyCommand(shellState, jcut::SetTranscriptVisibleCommand{transcript});
            }
            bool scopes = snapshot.panels.showScopes;
            if (ImGui::Checkbox("Enable Scopes", &scopes)) {
                applyCommand(shellState, jcut::SetScopesVisibleCommand{scopes});
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void drawStatusBar(const ShellState& shellState, const jcut::EditorDocumentCore& snapshot)
{
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float height = 28.0f;
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y + viewport->Size.y - height));
    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, height));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
    ImGui::Begin("StatusBar", nullptr, flags);
    ImGui::TextUnformatted(snapshot.transport.playbackActive ? "Playing" : "Idle");
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::Text("Export %dx%d @ %.2f",
                snapshot.exportRequest.outputSize.width,
                snapshot.exportRequest.outputSize.height,
                snapshot.exportRequest.outputFps);
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::TextUnformatted(shellState.documentPath.empty()
        ? (shellState.usesQtProjectStorage ? shellState.projectId.c_str() : "Shell: ImGui demo")
        : shellState.documentPath.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::TextUnformatted(shellState.statusMessage.c_str());
    if (!shellState.audioStatusMessage.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::TextUnformatted(shellState.audioStatusMessage.c_str());
    }
    ImGui::End();
}

} // namespace

int main(int argc, char** argv)
{
    ShellState shellState;
    if (argc > 2) {
        std::fprintf(stderr, "usage: %s [state-or-core-json]\n", argv[0]);
        return 1;
    }
    if (argc == 2) {
        std::string error;
        const std::optional<jcut::EditorDocumentCore> loadedDocument =
            jcut::loadEditorDocumentCoreFromFile(argv[1], &error);
        if (!loadedDocument.has_value()) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        {
            std::lock_guard<std::mutex> lock(shellState.runtimeMutex);
            shellState.runtime = jcut::EditorRuntime::fromDocument(*loadedDocument);
        }
        shellState.documentPath = argv[1];
        shellState.projectRootPath = pathString(fs::path(shellState.documentPath).parent_path());
        shellState.preferencesPath = pathString(fs::path(shellState.documentPath).parent_path() /
                                                (fs::path(shellState.documentPath).filename().string() +
                                                 ".imgui_prefs.json"));
        shellState.lastSavedSnapshotJson = snapshotJson(*loadedDocument);
        shellState.statusMessage = "document loaded";
        std::snprintf(shellState.exportOutputPath.data(),
                      shellState.exportOutputPath.size(),
                      "%s",
                      loadedDocument->exportRequest.outputPath.c_str());
    } else {
        std::string error;
        const std::optional<jcut::ImGuiProjectSession> session =
            jcut::loadActiveImGuiProjectSession(&error);
        if (!session.has_value()) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        {
            std::lock_guard<std::mutex> lock(shellState.runtimeMutex);
            shellState.runtime = jcut::EditorRuntime::fromDocument(session->document);
        }
        shellState.projectId = session->projectId;
        shellState.statePath = session->statePath;
        shellState.historyPath = session->historyPath;
        shellState.projectRootPath = session->rootDirPath;
        shellState.preferencesPath = pathString(fs::path(shellState.statePath).parent_path() /
                                                "imgui_prefs.json");
        shellState.legacyStateRoot = session->legacyStateRoot;
        shellState.usesQtProjectStorage = true;
        shellState.lastSavedSnapshotJson = snapshotJson(session->document);
        shellState.statusMessage = "active Qt project loaded";
        std::snprintf(shellState.exportOutputPath.data(),
                      shellState.exportOutputPath.size(),
                      "%s",
                      session->document.exportRequest.outputPath.c_str());
    }
    loadUiPreferences(&shellState);

    X11Platform platform;
    std::string platformError;
    if (!platform.create(1600, 960, "JCut ImGui", &platformError)) {
        std::fprintf(stderr, "%s\n", platformError.empty()
            ? "failed to create X11 window"
            : platformError.c_str());
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    loadUiFont(&shellState);
    applyShellStyle();
    VulkanShell vulkanShell;
    std::string vulkanError;
    if (!vulkanShell.initialize(&platform, &vulkanError)) {
        std::fprintf(stderr, "%s\n", vulkanError.empty()
            ? "failed to initialize Vulkan ImGui shell"
            : vulkanError.c_str());
        ImGui::DestroyContext();
        platform.shutdown();
        return 1;
    }
    std::string controlServerError;
    const std::uint16_t controlPort = jcut::runtimeControlPortFromEnvironment(40130);
    if (shellState.controlServer.start(
            controlPort,
            jcut::RuntimeControlProvider{
                [&shellState]() { return runtimeControlSnapshot(&shellState); },
                [&shellState]() { return runtimeControlScreenshot(&shellState); },
                [&shellState](std::int64_t frame, std::string* error) {
                    return setRuntimeControlPlayhead(&shellState, frame, error);
                }},
            &controlServerError)) {
        shellState.statusMessage = "control API listening on 127.0.0.1:" + std::to_string(controlPort);
    } else if (!controlServerError.empty()) {
        shellState.statusMessage = "control API unavailable: " + controlServerError;
    }
    shellState.previewWorker = std::thread(runPreviewWorker, &shellState);
    shellState.exportWorker = std::thread(runExportWorker, &shellState);
    requestPreviewRender(&shellState);
    auto previousTick = std::chrono::steady_clock::now();

    while (!platform.shouldClose()) {
        platform.pollEvents();
        const auto now = std::chrono::steady_clock::now();
        const std::chrono::duration<double> delta = now - previousTick;
        previousTick = now;
        int previousFrame = 0;
        int currentFrame = 0;
        {
            std::lock_guard<std::mutex> lock(shellState.runtimeMutex);
            previousFrame = shellState.runtime.snapshot().transport.currentFrame;
            shellState.runtime.tick({delta.count()});
            currentFrame = shellState.runtime.snapshot().transport.currentFrame;
        }
        if (currentFrame != previousFrame) {
            requestPreviewRender(&shellState);
        }
        uploadPreviewTexture(&shellState, &vulkanShell);

        const bool savePressed = platform.savePressed();
        const bool reloadPressed = platform.reloadPressed();
        const bool fontSizeIncreasePressed = platform.fontIncreasePressed();
        const bool fontSizeDecreasePressed = platform.fontDecreasePressed();
        if (savePressed && !shellState.saveShortcutPressed) {
            saveCurrentDocument(&shellState);
        }
        if (reloadPressed && !shellState.reloadShortcutPressed) {
            reloadCurrentDocument(&shellState);
        }
        if (fontSizeIncreasePressed && !shellState.fontSizeIncreaseShortcutPressed) {
            changeUiFontSize(&shellState, 1.0f);
        }
        if (fontSizeDecreasePressed && !shellState.fontSizeDecreaseShortcutPressed) {
            changeUiFontSize(&shellState, -1.0f);
        }
        shellState.saveShortcutPressed = savePressed;
        shellState.reloadShortcutPressed = reloadPressed;
        shellState.fontSizeIncreaseShortcutPressed = fontSizeIncreasePressed;
        shellState.fontSizeDecreaseShortcutPressed = fontSizeDecreasePressed;

        {
            std::lock_guard<std::mutex> lock(shellState.exportMutex);
            if (shellState.exportCompletedGeneration > shellState.exportStatusGeneration) {
                shellState.exportStatusGeneration = shellState.exportCompletedGeneration;
                shellState.statusMessage = shellState.exportResult.message.empty()
                    ? "export finished"
                    : shellState.exportResult.message;
            }
        }

        vulkanShell.rebuildSwapchainIfNeeded();
        ImGui_ImplVulkan_NewFrame();
        platform.newFrame();
        ImGui::NewFrame();

        jcut::EditorDocumentCore snapshot;
        {
            std::lock_guard<std::mutex> lock(shellState.runtimeMutex);
            snapshot = shellState.runtime.snapshot();
        }
        syncAudioEngine(&shellState, snapshot);
        drawMenuBar(&shellState, snapshot);
        drawMediaPanel(&shellState, snapshot);
        drawPreviewPanel(&shellState, snapshot);
        drawTimelinePanel(&shellState, snapshot);
        drawInspectorPanel(&shellState, snapshot);
        drawStatusBar(shellState, snapshot);

        ImGui::Render();
        vulkanShell.renderDrawData(ImGui::GetDrawData());
        vulkanShell.present();
    }

    vulkanShell.shutdown();
    shellState.audioEngine.stop();
    shellState.audioEngine.shutdown();
    ImGui::DestroyContext();
    {
        std::lock_guard<std::mutex> lock(shellState.previewMutex);
        shellState.previewStopRequested = true;
    }
    shellState.previewCondition.notify_one();
    {
        std::lock_guard<std::mutex> lock(shellState.exportMutex);
        shellState.exportStopRequested = true;
        shellState.exportCancelRequested = true;
    }
    shellState.exportCondition.notify_one();
    if (shellState.previewWorker.joinable()) {
        shellState.previewWorker.join();
    }
    if (shellState.exportWorker.joinable()) {
        shellState.exportWorker.join();
    }
    shellState.controlServer.stop();
    platform.shutdown();
    return 0;
}
