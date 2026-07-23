#include "editor_runtime.h"
#include "editor_document_core_json.h"
#include "editor_grading_core.h"
#include "editor_scale_to_fill.h"
#include "face_artifact_core.h"
#include "face_processing_job_core.h"
#include "imgui_audio_runtime.h"
#include "image_sequence_directory.h"
#include "mask_sidecar_core.h"
#include "imgui_project_io.h"
#include "imgui_vulkan_frame_importer.h"
#include "preview_resize_core.h"
#include "proxy_path_core.h"
#include "proxy_generation_job_core.h"
#include "render_contract_json.h"
#include "runtime_control_server.h"
#include "speaker_title_core.h"
#include "standalone_export_renderer.h"
#include "standalone_preview_renderer.h"
#include "standalone_timeline_renderer.h"
#include "transcript_cut_session_core.h"
#include "transcript_document_mutation_core.h"
#include "transcript_mining_core.h"

#include "external/imgui/imgui.h"
#include "external/imgui/backends/imgui_impl_vulkan.h"
#include "external/imgui/misc/cpp/imgui_stdlib.h"

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
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <mutex>
#include <limits>
#include <system_error>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
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

enum class PreviewTransformDragMode {
    None,
    Move,
    ResizeX,
    ResizeY,
    ResizeBoth,
};

enum class TimelineToolMode {
    Select,
    Razor,
};

enum class ProjectLifecycleAction {
    None,
    NewProject,
    SaveAs,
    Rename,
};

constexpr float kTimelinePixelsPerFrame = 0.35f;
constexpr float kTimelineRowHeight = 34.0f;
constexpr float kTimelineLabelWidth = 90.0f;
constexpr float kTimelineClipHeight = 24.0f;
constexpr float kTimelineTrackPadding = 12.0f;
constexpr float kTimelineTopPadding = 10.0f;
constexpr float kTimelineHandleWidth = 8.0f;
constexpr float kTimelineSnapThresholdPixels = 10.0f;
constexpr char kProjectMediaDragPayload[] = "JCUT_MEDIA_ITEM";
constexpr char kFilesystemMediaDragPayload[] = "JCUT_MEDIA_PATH";
constexpr char kGeneratedTrackLabelPrefix[] = "  [child] ";
constexpr ImU32 kGeneratedTrackLaneColor = IM_COL32(27, 30, 35, 255);
constexpr ImU32 kGeneratedTrackClipColor = IM_COL32(72, 82, 92, 255);
constexpr ImU32 kGeneratedTrackSelectedClipColor = IM_COL32(190, 158, 104, 255);
constexpr float kShellGap = 10.0f;
constexpr float kStatusBarHeight = 28.0f;
constexpr float kMediaPanelWidth = 320.0f;
constexpr float kInspectorPanelWidth = 340.0f;
constexpr float kTimelinePanelHeight = 280.0f;
constexpr float kDefaultUiFontSize = 16.0f;
constexpr float kMinUiFontSize = 11.0f;
constexpr float kMaxUiFontSize = 28.0f;

using InspectorKeyframeValue = std::variant<
    std::monostate,
    jcut::EditorGradingKeyframe,
    jcut::EditorOpacityKeyframe,
    jcut::EditorTransformKeyframe>;

struct InspectorKeyframeDraft {
    int clipId = -1;
    jcut::EditorKeyframeChannel channel = jcut::EditorKeyframeChannel::Grading;
    std::int64_t originalFrame = 0;
    bool existing = false;
    InspectorKeyframeValue value;
};

struct TranscriptInspectorCache {
    int clipId = -1;
    std::string sourceKey;
    std::string requestedPath;
    jcut::TranscriptTiming timing;
    bool includeOutsideCut = false;
    bool loaded = false;
    bool refreshRequested = true;
    std::chrono::steady_clock::time_point nextFilesystemCheck{};
    jcut::TranscriptCutSession session;
    jcut::TranscriptWordRef selectedWord;
    std::string selectedText;
    double selectedStartSeconds = 0.0;
    double selectedEndSeconds = 0.0;
    bool selectedSkipped = false;
    bool selectionDraftValid = false;
    std::string mutationError;
    std::string speakerFilter;
    std::string searchFilter;
    std::string cutLabelPath;
    std::string cutLabelDraft;
    std::string selectedSpeakerId;
    std::string speakerNameDraft;
    std::string speakerOrganizationDraft;
    double speakerXDraft = 0.5;
    double speakerYDraft = 0.85;
    std::string faceArtifactContext;
    jcut::FaceArtifactInspectionCore faceInspection;
    std::vector<int> selectedFaceTrackIds;
    int faceJobLastState = -1;
    int faceJobStride = 1;
    int faceJobWorkers = 2;
    int faceJobPipelineSlots = 2;
    double faceJobThreshold = 0.5;
    bool faceJobPrimaryOnly = false;
    bool faceJobSmallFaceFallback = true;
    bool faceJobTiling = false;
    bool faceJobAllowCpuFallback = true;
    int speakerTitleStyle = 0;
    double speakerTitleDurationSeconds = 3.0;
    double speakerTitleDelaySeconds = 0.35;
    double speakerTitleFlySeconds = 0.35;
    bool speakerTitleShowOrganization = true;
    std::vector<jcut::TranscriptMiningProposal> miningProposals;
    std::vector<std::uint8_t> miningProposalSelected;
    std::string miningProposalLabel;
};

struct RenderSyncMarkerDraft {
    int clipId = 0;
    std::string clipPersistentId;
    std::int64_t frame = 0;
    bool skipFrame = false;
    int count = 1;
    bool popupRequested = false;
    std::uint64_t documentGeneration = 0;
};

struct ShellState {
    jcut::EditorRuntime runtime;
    std::mutex runtimeMutex;
    jcut::RuntimeControlServer controlServer;
    std::string documentPath;
    std::string projectId;
    std::string statePath;
    std::string historyPath;
    std::string projectRootPath;
    std::string mediaRootDirectory;
    std::uint64_t documentGeneration = 1;
    std::string lastSavedSnapshotJson;
    std::string statusMessage;
    jcut::ImGuiAudioRuntime audioRuntime;
    jcut::FaceProcessingJobController faceProcessingJob;
    jcut::ProxyGenerationJobController proxyGenerationJob;
    nlohmann::json legacyStateRoot;
    nlohmann::json legacyStateOverrides = nlohmann::json::object();
    std::string lastSavedLegacyExtensionSignature;
    bool usesQtProjectStorage = false;
    bool focusMediaFilesRequested = false;
    bool focusInspectorOutputRequested = false;
    bool focusInspectorProjectsRequested = false;
    bool resetLayoutRequested = false;
    bool closeConfirmationRequested = false;
    float uiFontSize = kDefaultUiFontSize;
    float loadedUiFontSize = kDefaultUiFontSize;
    std::string preferencesPath;
    std::string layoutIniPath;
    std::string uiFontPath;
    std::array<char, 512> importMediaPath{};
    std::array<char, 128> importMediaLabel{};
    std::array<char, 64> importMediaKind{};
    std::array<char, 512> mediaRootPath{};
    std::array<char, 128> mediaBrowserFilter{};
    std::array<char, 128> projectNameDraft{};
    ProjectLifecycleAction projectLifecycleAction = ProjectLifecycleAction::None;
    bool projectLifecyclePopupRequested = false;
    std::vector<jcut::ImGuiProjectHistoryEntry> projectHistoryEntries;
    std::string projectHistoryError;
    bool projectHistoryRefreshRequested = true;
    std::string mediaGalleryPath;
    std::string mediaHoveredPath;
    std::string mediaSelectedPath;
    std::array<char, 512> exportOutputPath{};
    int proxyPathDraftClipId = -1;
    std::string proxyPathDraft;
    bool overwriteProxyGeneration = false;
    int proxyGenerationFormatIndex = 0;
    int autosaveIntervalMinutes = 5;
    int autosaveMaxBackups = 20;
    int historyMaxEntries = 100;
    int historyMaxMegabytes = 16;
    int audioBufferFrames = 1024;
    std::chrono::steady_clock::time_point nextAutosaveAt =
        std::chrono::steady_clock::now() + std::chrono::minutes(5);
    int titleDraftClipId = -1;
    jcut::EditorTitleKeyframe titleDraft;
    InspectorKeyframeDraft keyframeDraft;
    TranscriptInspectorCache transcriptCache;
    std::unordered_map<std::string, jcut::TranscriptFileStamp>
        transcriptHistoryExpectedStamps;
    RenderSyncMarkerDraft renderSyncMarkerDraft;
    TimelineDragMode timelineDragMode = TimelineDragMode::None;
    TimelineToolMode timelineToolMode = TimelineToolMode::Select;
    bool timelineSnappingEnabled = true;
    float trackCrossfadeSeconds = 0.5f;
    bool trackCrossfadeMoveClips = false;
    int timelineSnapIndicatorFrame = -1;
    int timelineDragClipId = 0;
    int timelineDragTrackId = 0;
    int timelineDragTrackIndex = -1;
    int timelineDragStartFrame = 0;
    int timelineDragDurationFrames = 0;
    int timelineContextClipId = 0;
    std::string timelineContextClipPersistentId;
    std::int64_t timelineContextFrame = 0;
    std::uint64_t timelineContextDocumentGeneration = 0;
    float timelineDragMouseX = 0.0f;
    float timelineDragMouseY = 0.0f;
    bool historyTransactionActive = false;
    std::mutex previewMutex;
    std::condition_variable previewCondition;
    std::thread previewWorker;
    bool previewStopRequested = false;
    bool previewRenderRequested = false;
    std::atomic<bool> uiPreviewRefreshRequested{false};
    std::uint64_t previewRequestGeneration = 0;
    std::uint64_t previewCompletedGeneration = 0;
    std::uint64_t previewUploadedGeneration = 0;
    bool previewLastUsedZeroCopy = false;
    bool previewZeroCopyAvailable = false;
    std::string previewZeroCopyFailureReason;
    bool previewCpuFallbackPreferred = false;
    float previewPanX = 0.0f;
    float previewPanY = 0.0f;
    bool previewTitleDragActive = false;
    int previewTitleDragClipId = -1;
    jcut::EditorTitleKeyframe previewTitleDragKeyframe;
    PreviewTransformDragMode previewTransformDragMode = PreviewTransformDragMode::None;
    int previewTransformDragClipId = -1;
    ImVec2 previewTransformDragOriginMouse{};
    ImVec2 previewTransformDragOriginBoundsMin{};
    ImVec2 previewTransformDragOriginBoundsMax{};
    jcut::EditorTransformKeyframe previewTransformDragValue;
    bool correctionDrawMode = false;
    int correctionClipId = -1;
    int selectedCorrectionPolygon = -1;
    std::vector<jcut::EditorPoint> correctionDraftPoints;
    bool correctionPointDragActive = false;
    int correctionPointDragPolygon = -1;
    int correctionPointDragPoint = -1;
    std::vector<jcut::EditorCorrectionPolygon> correctionPointDragPolygons;
    int maskSidecarContextClipId = -1;
    std::string maskSidecarDirectoryDraft;
    std::vector<jcut::masks::MaskSidecarCore> maskSidecars;
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

std::string lowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool parseHexRgbColor(const std::string& value, std::array<float, 3>* color)
{
    if (!color || value.empty()) {
        return false;
    }
    const std::size_t prefix = value.front() == '#' ? 1 : 0;
    std::size_t componentOffset = prefix;
    const std::size_t digitCount = value.size() - prefix;
    if (digitCount == 8) {
        // QColor's eight-digit form is #AARRGGBB. The neutral overlay stores
        // opacity separately, so keep only the RGB portion here.
        componentOffset += 2;
    } else if (digitCount != 6 && digitCount != 3) {
        return false;
    }

    const auto hexDigit = [](char digit) -> int {
        if (digit >= '0' && digit <= '9') {
            return digit - '0';
        }
        if (digit >= 'a' && digit <= 'f') {
            return digit - 'a' + 10;
        }
        if (digit >= 'A' && digit <= 'F') {
            return digit - 'A' + 10;
        }
        return -1;
    };
    for (std::size_t component = 0; component < color->size(); ++component) {
        int byte = 0;
        if (digitCount == 3) {
            const int nibble = hexDigit(value[componentOffset + component]);
            if (nibble < 0) {
                return false;
            }
            byte = nibble * 17;
        } else {
            const std::size_t offset = componentOffset + (component * 2);
            const int high = hexDigit(value[offset]);
            const int low = hexDigit(value[offset + 1]);
            if (high < 0 || low < 0) {
                return false;
            }
            byte = (high * 16) + low;
        }
        (*color)[component] = static_cast<float>(byte) / 255.0f;
    }
    return true;
}

std::string formatHexRgbColor(const std::array<float, 3>& color)
{
    std::array<int, 3> bytes{};
    std::transform(color.begin(), color.end(), bytes.begin(), [](float component) {
        return static_cast<int>(std::lround(std::clamp(component, 0.0f, 1.0f) * 255.0f));
    });
    char value[8]{};
    std::snprintf(value, sizeof(value), "#%02x%02x%02x",
                  bytes[0], bytes[1], bytes[2]);
    return value;
}

bool editHexRgbColor(const char* label,
                     std::string* value,
                     const char* fallback)
{
    if (!value) {
        return false;
    }
    std::array<float, 3> color{};
    if (!parseHexRgbColor(*value, &color) &&
        !parseHexRgbColor(fallback ? fallback : "#ffffff", &color)) {
        color = {1.0f, 1.0f, 1.0f};
    }
    if (!ImGui::ColorEdit3(label, color.data())) {
        return false;
    }
    *value = formatHexRgbColor(color);
    return true;
}

std::string mediaKindForPath(const fs::path& path)
{
    std::error_code directoryError;
    if (fs::is_directory(path, directoryError) && !directoryError &&
        jcut::isImageSequenceDirectory(path)) {
        return "video";
    }
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

jcut::ImportMediaCommand importMediaCommandForPath(
    const std::string& sourcePath,
    const std::string& label,
    const std::string& requestedKind,
    std::int64_t* probedDurationFrames = nullptr)
{
    const jcut::standalone_render::StandaloneMediaInfo mediaInfo =
        jcut::standalone_render::probeStandaloneMedia(sourcePath);
    if (probedDurationFrames) {
        *probedDurationFrames = mediaInfo.probed
            ? std::max<std::int64_t>(0, mediaInfo.durationFrames)
            : 0;
    }
    std::string mediaKind = requestedKind;
    if ((mediaKind.empty() || mediaKind == "media" || mediaKind == "unknown") &&
        mediaInfo.probed) {
        mediaKind = mediaInfo.mediaKind;
    }
    return {
        sourcePath,
        label,
        mediaKind.empty() ? std::string("unknown") : mediaKind,
        mediaInfo.probed,
        mediaInfo.hasAudio};
}

bool isMediaFilePath(const fs::path& path)
{
    return mediaKindForPath(path) != "media";
}

bool isImportableMediaPath(const fs::path& path)
{
    std::error_code error;
    if (fs::is_regular_file(path, error) && !error) {
        return isMediaFilePath(path);
    }
    error.clear();
    return fs::is_directory(path, error) && !error &&
        jcut::isImageSequenceDirectory(path);
}

bool clipCanScaleToFill(const jcut::EditorClip& clip)
{
    if (clip.locked || clip.sourcePath.empty()) {
        return false;
    }
    const std::string kind = lowerAscii(clip.mediaKind);
    if (kind == "audio" || kind == "title" || kind == "graphics") {
        return false;
    }
    if (kind == "video" || kind == "image") {
        return true;
    }
    const std::string inferredKind = mediaKindForPath(fs::path(clip.sourcePath));
    return inferredKind == "video" || inferredKind == "image";
}

std::string displayNameForPath(const fs::path& path)
{
    std::error_code error;
    if (fs::is_directory(path, error) && !error) {
        return path.filename().string();
    }
    const std::string stem = path.stem().string();
    if (!stem.empty()) {
        return stem;
    }
    return path.filename().string();
}

int resolvedMediaDurationFrames(int requestedDurationFrames,
                                std::int64_t probedDurationFrames)
{
    return requestedDurationFrames > 0
        ? requestedDurationFrames
        : static_cast<int>(std::clamp<std::int64_t>(
              probedDurationFrames > 0 ? probedDurationFrames : 90,
              1,
              std::numeric_limits<int>::max()));
}

jcut::AddClipCommand addClipCommandForPath(const fs::path& path,
                                           int trackId,
                                           int startFrame,
                                           int durationFrames = 0)
{
    const std::string normalizedPath = pathString(path);
    std::int64_t probedDurationFrames = 0;
    const jcut::ImportMediaCommand media = importMediaCommandForPath(
        normalizedPath,
        displayNameForPath(path),
        mediaKindForPath(path),
        &probedDurationFrames);
    const int resolvedDuration = resolvedMediaDurationFrames(
        durationFrames, probedDurationFrames);
    return {
        trackId,
        media.label,
        startFrame,
        resolvedDuration,
        media.sourcePath,
        media.mediaKind,
        media.audioPresenceKnown,
        media.hasAudio};
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
    if (ec) {
        return false;
    }
    const fs::path tempPath = path.string() + ".tmp-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
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
        std::error_code removeError;
        fs::remove(tempPath, removeError);
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
        shellState->audioBufferFrames = std::clamp(
            root.value("audioBufferFrames", 1024), 64, 8192);
    } catch (...) {
        shellState->uiFontSize = kDefaultUiFontSize;
        shellState->audioBufferFrames = 1024;
    }
}

void saveUiPreferences(const ShellState& shellState)
{
    if (shellState.preferencesPath.empty()) {
        return;
    }
    const nlohmann::json root{
        {"uiFontSize", shellState.uiFontSize},
        {"audioBufferFrames", shellState.audioBufferFrames},
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
        shellState->previewRootDirectory = shellState->mediaRootDirectory.empty()
            ? shellState->projectRootPath
            : shellState->mediaRootDirectory;
        shellState->previewRenderRequested = true;
        ++shellState->previewRequestGeneration;
    }
    shellState->previewCondition.notify_one();
}

bool commitExportOutputPathDraft(ShellState* shellState)
{
    const std::string draftPath = shellState->exportOutputPath.data();
    jcut::CommandResult result;
    {
        std::lock_guard<std::mutex> lock(shellState->runtimeMutex);
        if (shellState->runtime.snapshot().exportRequest.outputPath == draftPath) {
            return true;
        }
        result = shellState->runtime.execute(jcut::EditorCommand{
            jcut::SetExportOutputPathCommand{draftPath}});
    }
    shellState->statusMessage = result.message;
    if (result.applied) {
        requestPreviewRender(shellState);
    }
    return result.applied;
}

bool requestExportRender(ShellState* shellState)
{
    if (!commitExportOutputPathDraft(shellState)) {
        return false;
    }
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
    shellState->exportRootDirectory = shellState->mediaRootDirectory.empty()
        ? shellState->projectRootPath
        : shellState->mediaRootDirectory;
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
bool documentIsDirty(const ShellState& shellState, const jcut::EditorDocumentCore& snapshot);

bool synchronizeTranscriptHistoryNavigation(
    ShellState* shellState,
    const jcut::EditorDocumentCore& before,
    const jcut::EditorDocumentCore& after,
    std::string* errorOut)
{
    if (errorOut) errorOut->clear();
    for (const auto& document : after.transcriptHistoryDocuments) {
        const auto previous = std::find_if(
            before.transcriptHistoryDocuments.begin(),
            before.transcriptHistoryDocuments.end(),
            [&](const auto& candidate) { return candidate.path == document.path; });
        if (previous != before.transcriptHistoryDocuments.end() &&
            previous->jsonPayload == document.jsonPayload) {
            continue;
        }
        const jcut::TranscriptFileStamp current =
            jcut::inspectTranscriptFile(document.path);
        const auto expected =
            shellState->transcriptHistoryExpectedStamps.find(document.path);
        if (expected != shellState->transcriptHistoryExpectedStamps.end() &&
            current != expected->second) {
            if (errorOut) {
                *errorOut =
                    "Transcript changed outside JCut; undo/redo was cancelled.";
            }
            return false;
        }
        nlohmann::json root;
        try {
            root = nlohmann::json::parse(document.jsonPayload);
        } catch (const nlohmann::json::exception& exception) {
            if (errorOut) {
                *errorOut = std::string("Invalid transcript history payload: ") +
                    exception.what();
            }
            return false;
        }
        std::string saveError;
        if (!jcut::saveTranscriptDocumentAtomic(
                document.path, root, &saveError)) {
            if (errorOut) *errorOut = std::move(saveError);
            return false;
        }
        shellState->transcriptHistoryExpectedStamps[document.path] =
            jcut::inspectTranscriptFile(document.path);
        if (shellState->transcriptCache.session.activePath == document.path) {
            shellState->transcriptCache.selectionDraftValid = false;
            shellState->transcriptCache.refreshRequested = true;
        }
    }
    return true;
}

template <typename Command>
jcut::CommandResult applyCommand(ShellState* shellState, Command&& command)
{
    using CommandType = std::decay_t<Command>;
    constexpr bool historyNavigation =
        std::is_same_v<CommandType, jcut::UndoCommand> ||
        std::is_same_v<CommandType, jcut::RedoCommand>;
    jcut::EditorDocumentCore before;
    if constexpr (historyNavigation) {
        std::lock_guard<std::mutex> lock(shellState->runtimeMutex);
        before = shellState->runtime.snapshot();
    }
    jcut::CommandResult result =
        [&]() {
            std::lock_guard<std::mutex> lock(shellState->runtimeMutex);
            return shellState->runtime.execute(jcut::EditorCommand{std::forward<Command>(command)});
        }();
    if constexpr (historyNavigation) {
        if (result.applied) {
            jcut::EditorDocumentCore after;
            {
                std::lock_guard<std::mutex> lock(shellState->runtimeMutex);
                after = shellState->runtime.snapshot();
            }
            std::string syncError;
            if (!synchronizeTranscriptHistoryNavigation(
                    shellState, before, after, &syncError)) {
                std::lock_guard<std::mutex> lock(shellState->runtimeMutex);
                if constexpr (std::is_same_v<CommandType, jcut::UndoCommand>) {
                    (void)shellState->runtime.execute(
                        jcut::EditorCommand{jcut::RedoCommand{}});
                } else {
                    (void)shellState->runtime.execute(
                        jcut::EditorCommand{jcut::UndoCommand{}});
                }
                result = {false, std::move(syncError)};
            }
        }
    }
    shellState->statusMessage = result.message;
    requestPreviewRender(shellState);
    return result;
}

fs::path resolvedClipMediaPathForProbe(const ShellState& shellState,
                                       const jcut::EditorClip& clip)
{
    const fs::path mediaRoot = !shellState.mediaRootDirectory.empty()
        ? fs::path(shellState.mediaRootDirectory)
        : fs::path(shellState.projectRootPath);
    const auto resolve = [&mediaRoot](const std::string& value) {
        fs::path path(value);
        if (path.is_relative() && !mediaRoot.empty()) {
            path = mediaRoot / path;
        }
        return path.lexically_normal();
    };

    if (clip.useProxy && !clip.proxyPath.empty()) {
        const fs::path proxyPath = resolve(clip.proxyPath);
        if (isImportableMediaPath(proxyPath)) {
            return proxyPath;
        }
    }
    return resolve(clip.sourcePath);
}

void scaleClipToFillPreview(ShellState* shellState,
                            const jcut::EditorDocumentCore& snapshot,
                            const jcut::EditorClip& clip)
{
    if (!clipCanScaleToFill(clip)) {
        shellState->statusMessage = "scale to fill unavailable for this clip";
        return;
    }
    if (!snapshot.exportRequest.outputSize.valid()) {
        shellState->statusMessage = "scale to fill unavailable: invalid preview size";
        return;
    }

    const fs::path mediaPath = resolvedClipMediaPathForProbe(*shellState, clip);
    const jcut::standalone_render::StandaloneMediaInfo mediaInfo =
        jcut::standalone_render::probeStandaloneMedia(pathString(mediaPath));
    if (!mediaInfo.probed || !mediaInfo.hasVideo || !mediaInfo.frameSize.valid()) {
        shellState->statusMessage = "scale to fill unavailable: source dimensions could not be read";
        return;
    }

    const std::optional<double> fillScale = jcut::scaleToFillFactor(
        mediaInfo.frameSize, snapshot.exportRequest.outputSize);
    if (!fillScale.has_value()) {
        shellState->statusMessage = "scale to fill unavailable: invalid source dimensions";
        return;
    }

    applyCommand(shellState, jcut::SetClipTransformCommand{
        clip.id,
        0.0,
        0.0,
        clip.baseRotation,
        *fillScale,
        *fillScale});
}

jcut::EditorDocumentCore runtimeSnapshot(ShellState* shellState)
{
    std::lock_guard<std::mutex> lock(shellState->runtimeMutex);
    return shellState->runtime.snapshot();
}

void beginRuntimeHistoryTransaction(ShellState* shellState)
{
    if (shellState->historyTransactionActive) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(shellState->runtimeMutex);
        shellState->runtime.beginHistoryTransaction();
    }
    shellState->historyTransactionActive = true;
}

void beginRuntimeHistoryTransactionForLastItem(ShellState* shellState)
{
    if (ImGui::IsItemActivated()) {
        beginRuntimeHistoryTransaction(shellState);
    }
}

void endRuntimeHistoryTransaction(ShellState* shellState)
{
    if (!shellState->historyTransactionActive) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(shellState->runtimeMutex);
        shellState->runtime.endHistoryTransaction();
    }
    shellState->historyTransactionActive = false;
}

void finishRuntimeHistoryTransactionIfIdle(ShellState* shellState)
{
    if (!shellState->historyTransactionActive || ImGui::IsAnyItemActive() ||
        shellState->timelineDragMode != TimelineDragMode::None ||
        shellState->previewTitleDragActive) {
        return;
    }
    endRuntimeHistoryTransaction(shellState);
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

const jcut::EditorClip* clipForPersistentId(
    const jcut::EditorDocumentCore& snapshot,
    const std::string& persistentId)
{
    const auto clip = std::find_if(
        snapshot.clips.begin(), snapshot.clips.end(),
        [&](const jcut::EditorClip& candidate) {
            return candidate.persistentId == persistentId;
        });
    return clip == snapshot.clips.end() ? nullptr : &*clip;
}

const jcut::EditorRenderSyncMarker* renderSyncMarkerForClipAtFrame(
    const jcut::EditorDocumentCore& snapshot,
    const jcut::EditorClip& clip,
    std::int64_t frame)
{
    const std::string ownerClipId = jcut::editorRenderSyncOwnerClipId(
        snapshot, clip.persistentId);
    const auto marker = std::find_if(
        snapshot.renderSyncMarkers.begin(), snapshot.renderSyncMarkers.end(),
        [&](const jcut::EditorRenderSyncMarker& candidate) {
            return candidate.clipId == ownerClipId &&
                candidate.frame == frame;
        });
    return marker == snapshot.renderSyncMarkers.end() ? nullptr : &*marker;
}

void requestRenderSyncMarkerCount(
    ShellState* shellState,
    const jcut::EditorClip& clip,
    std::int64_t frame,
    const jcut::EditorRenderSyncMarker* currentMarker,
    bool skipFrame)
{
    RenderSyncMarkerDraft& draft = shellState->renderSyncMarkerDraft;
    draft.clipId = clip.id;
    draft.clipPersistentId = clip.persistentId;
    draft.frame = frame;
    draft.skipFrame = skipFrame;
    draft.count = currentMarker && currentMarker->skipFrame == skipFrame
        ? currentMarker->count
        : 1;
    draft.popupRequested = true;
    draft.documentGeneration = shellState->documentGeneration;
}

void drawRenderSyncContextActions(
    ShellState* shellState,
    const jcut::EditorDocumentCore& snapshot,
    const jcut::EditorClip& clip,
    std::int64_t frame)
{
    const jcut::EditorRenderSyncMarker* currentMarker =
        renderSyncMarkerForClipAtFrame(snapshot, clip, frame);
    if (ImGui::MenuItem("Duplicate Frames For Clip...")) {
        requestRenderSyncMarkerCount(
            shellState, clip, frame, currentMarker, false);
    }
    if (ImGui::MenuItem("Skip Frames For Clip...")) {
        requestRenderSyncMarkerCount(
            shellState, clip, frame, currentMarker, true);
    }
    if (ImGui::MenuItem(
            "Clear At Playhead", nullptr, false, currentMarker != nullptr) &&
        currentMarker) {
        applyCommand(shellState, jcut::RemoveRenderSyncMarkerCommand{
            currentMarker->clipId,
            currentMarker->frame,
            currentMarker->skipFrame});
    }
}

std::size_t selectedClipCount(const jcut::EditorDocumentCore& snapshot)
{
    return static_cast<std::size_t>(std::count_if(
        snapshot.clips.begin(), snapshot.clips.end(),
        [](const jcut::EditorClip& clip) { return clip.selected; }));
}

void deleteSelectedClips(ShellState* shellState)
{
    applyCommand(shellState, jcut::DeleteSelectedClipsCommand{});
}

std::int64_t clipLocalPlayheadFrame(const jcut::EditorDocumentCore& snapshot,
                                    const jcut::EditorClip& clip)
{
    const std::int64_t localFrame =
        static_cast<std::int64_t>(snapshot.transport.currentFrame) - clip.startFrame;
    const std::int64_t lastFrame = std::max<std::int64_t>(0, clip.durationFrames - 1);
    return std::clamp(localFrame, std::int64_t{0}, lastFrame);
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

struct PreviewHistogram {
    static constexpr std::size_t kBinCount = 64;
    std::array<float, kBinCount> red{};
    std::array<float, kBinCount> green{};
    std::array<float, kBinCount> blue{};
    std::array<float, kBinCount> luma{};
    bool valid = false;
};

PreviewHistogram computePreviewHistogram(const jcut::core::ImageBuffer& image)
{
    PreviewHistogram histogram;
    if (image.empty() || image.strideBytes < image.size.width * 4) {
        return histogram;
    }
    const int sampleStep = std::max(1, std::min(image.size.width, image.size.height) / 256);
    float peak = 0.0f;
    for (int y = 0; y < image.size.height; y += sampleStep) {
        const std::size_t row = static_cast<std::size_t>(y) * image.strideBytes;
        for (int x = 0; x < image.size.width; x += sampleStep) {
            const std::size_t offset = row + static_cast<std::size_t>(x) * 4;
            if (offset + 2 >= image.bytes.size()) {
                continue;
            }
            const std::uint8_t red = image.bytes[offset];
            const std::uint8_t green = image.bytes[offset + 1];
            const std::uint8_t blue = image.bytes[offset + 2];
            const std::uint8_t luma = static_cast<std::uint8_t>(std::clamp(
                0.2126 * red + 0.7152 * green + 0.0722 * blue, 0.0, 255.0));
            const auto bin = [](std::uint8_t value) {
                return std::min<std::size_t>(PreviewHistogram::kBinCount - 1,
                                             static_cast<std::size_t>(value) *
                                                 PreviewHistogram::kBinCount / 256);
            };
            peak = std::max(peak, ++histogram.red[bin(red)]);
            peak = std::max(peak, ++histogram.green[bin(green)]);
            peak = std::max(peak, ++histogram.blue[bin(blue)]);
            peak = std::max(peak, ++histogram.luma[bin(luma)]);
        }
    }
    if (peak <= 0.0f) {
        return histogram;
    }
    for (std::size_t index = 0; index < PreviewHistogram::kBinCount; ++index) {
        histogram.red[index] /= peak;
        histogram.green[index] /= peak;
        histogram.blue[index] /= peak;
        histogram.luma[index] /= peak;
    }
    histogram.valid = true;
    return histogram;
}

PreviewHistogram currentPreviewHistogram(ShellState* shellState)
{
    std::lock_guard<std::mutex> lock(shellState->previewMutex);
    return computePreviewHistogram(shellState->previewResult.image);
}

void importFilesystemMedia(ShellState* shellState,
                           const jcut::EditorDocumentCore& snapshot,
                           const fs::path& path,
                           bool insertOnTimeline)
{
    if (!isImportableMediaPath(path)) {
        return;
    }
    const std::string normalizedPath = pathString(path);
    const std::string label = displayNameForPath(path);
    const std::string kind = mediaKindForPath(path);
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
            applyCommand(shellState, addClipCommandForPath(
                path, trackId, snapshot.transport.currentFrame));
        } else {
            shellState->statusMessage = "select a track before inserting media";
        }
    } else {
        applyCommand(shellState, importMediaCommandForPath(
            normalizedPath, label, kind));
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
    jcut::EditorDocumentCore persistentSnapshot = snapshot;
    persistentSnapshot.transport = {};
    persistentSnapshot.panels = {};
    for (jcut::EditorTrack& track : persistentSnapshot.tracks) {
        track.selected = false;
    }
    for (jcut::EditorClip& clip : persistentSnapshot.clips) {
        clip.selected = false;
    }
    return jcut::toJson(persistentSnapshot).dump();
}

std::string legacyStringValue(const ShellState& shellState, const char* key)
{
    const auto from = [key](const nlohmann::json& root) -> std::optional<std::string> {
        if (!root.is_object()) {
            return std::nullopt;
        }
        const auto value = root.find(key);
        if (value == root.end() || !value->is_string()) {
            return std::nullopt;
        }
        return value->get<std::string>();
    };
    if (const std::optional<std::string> value = from(shellState.legacyStateOverrides)) {
        return *value;
    }
    if (const std::optional<std::string> value = from(shellState.legacyStateRoot)) {
        return *value;
    }
    return {};
}

bool legacyBoolValue(const ShellState& shellState, const char* key, bool fallback)
{
    const auto from = [key](const nlohmann::json& root) -> std::optional<bool> {
        if (!root.is_object()) {
            return std::nullopt;
        }
        const auto value = root.find(key);
        if (value == root.end() || !value->is_boolean()) {
            return std::nullopt;
        }
        return value->get<bool>();
    };
    if (const std::optional<bool> value = from(shellState.legacyStateOverrides)) {
        return *value;
    }
    if (const std::optional<bool> value = from(shellState.legacyStateRoot)) {
        return *value;
    }
    return fallback;
}

int legacyIntValue(const ShellState& shellState,
                   const char* key,
                   int fallback,
                   int minimum,
                   int maximum)
{
    const auto from = [key](const nlohmann::json& root) -> std::optional<int> {
        if (!root.is_object()) return std::nullopt;
        const auto value = root.find(key);
        if (value == root.end() || !value->is_number_integer()) {
            return std::nullopt;
        }
        return value->get<int>();
    };
    if (const std::optional<int> value =
            from(shellState.legacyStateOverrides)) {
        return std::clamp(*value, minimum, maximum);
    }
    if (const std::optional<int> value = from(shellState.legacyStateRoot)) {
        return std::clamp(*value, minimum, maximum);
    }
    return std::clamp(fallback, minimum, maximum);
}

void reloadProjectPreferenceState(ShellState* shellState)
{
    shellState->autosaveIntervalMinutes = legacyIntValue(
        *shellState, "autosaveIntervalMinutes", 5, 1, 120);
    shellState->autosaveMaxBackups = legacyIntValue(
        *shellState, "autosaveMaxBackups", 20, 1, 200);
    shellState->historyMaxEntries = legacyIntValue(
        *shellState, "historyMaxEntries", 100, 10, 500);
    shellState->historyMaxMegabytes = legacyIntValue(
        *shellState, "historyMaxMegabytes", 16, 1, 256);
    shellState->nextAutosaveAt =
        std::chrono::steady_clock::now() +
        std::chrono::minutes(shellState->autosaveIntervalMinutes);
}

std::string legacyExtensionSignature(const ShellState& shellState)
{
    return nlohmann::json{
        {"mediaRoot", shellState.mediaRootDirectory},
        {"transcriptActiveCutPath",
         legacyStringValue(shellState, "transcriptActiveCutPath")},
        {"transcriptShowExcludedLines",
         legacyBoolValue(shellState, "transcriptShowExcludedLines", false)},
        {"autosaveIntervalMinutes", shellState.autosaveIntervalMinutes},
        {"autosaveMaxBackups", shellState.autosaveMaxBackups},
        {"historyMaxEntries", shellState.historyMaxEntries},
        {"historyMaxMegabytes", shellState.historyMaxMegabytes},
    }.dump();
}

void setLegacyStateOverride(ShellState* shellState,
                            const char* key,
                            nlohmann::json value)
{
    if (!shellState->legacyStateOverrides.is_object()) {
        shellState->legacyStateOverrides = nlohmann::json::object();
    }
    shellState->legacyStateOverrides[key] = std::move(value);
}

bool applyMediaRootPath(ShellState* shellState, const std::string& requestedPath)
{
    if (!shellState || requestedPath.empty()) {
        return false;
    }

    fs::path candidate(requestedPath);
    if (candidate.is_relative()) {
        candidate = fs::path(shellState->projectRootPath) / candidate;
    }
    std::error_code ec;
    if (!fs::is_directory(candidate, ec)) {
        shellState->statusMessage = "media root is not an existing directory";
        return false;
    }
    const fs::path canonical = fs::canonical(candidate, ec);
    const std::string mediaRoot = pathString(ec ? fs::absolute(candidate) : canonical);
    shellState->mediaRootDirectory = mediaRoot;
    std::snprintf(shellState->mediaRootPath.data(),
                  shellState->mediaRootPath.size(),
                  "%s",
                  mediaRoot.c_str());
    shellState->mediaGalleryPath.clear();
    setLegacyStateOverride(shellState, "mediaRoot", mediaRoot);
    setLegacyStateOverride(shellState, "explorerRoot", mediaRoot);
    shellState->statusMessage = "media root changed; save the project to keep it";
    requestPreviewRender(shellState);
    return true;
}

void commitLegacyStateOverrides(ShellState* shellState,
                                const jcut::EditorDocumentCore& document)
{
    shellState->legacyStateRoot = jcut::toLegacyStateJson(
        document, &shellState->legacyStateRoot);
    if (shellState->legacyStateOverrides.is_object() &&
        !shellState->legacyStateOverrides.empty()) {
        shellState->legacyStateRoot.merge_patch(shellState->legacyStateOverrides);
    }
    shellState->legacyStateOverrides = nlohmann::json::object();
    shellState->lastSavedLegacyExtensionSignature =
        legacyExtensionSignature(*shellState);
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
    bool previewLastUsedZeroCopy = false;
    bool previewZeroCopyAvailable = false;
    std::string previewZeroCopyFailureReason;
    {
        std::lock_guard<std::mutex> lock(shellState->previewMutex);
        previewResult = shellState->previewResult;
        previewCompletedGeneration = shellState->previewCompletedGeneration;
        previewLastUsedZeroCopy = shellState->previewLastUsedZeroCopy;
        previewZeroCopyAvailable = shellState->previewZeroCopyAvailable;
        previewZeroCopyFailureReason = shellState->previewZeroCopyFailureReason;
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
    const jcut::ImGuiAudioStatus audioStatus = shellState->audioRuntime.status();

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
            {"initialized", audioStatus.initialized},
            {"timelineConfigured", audioStatus.timelineConfigured},
            {"buffering", audioStatus.buffering},
            {"playbackActive", audioStatus.playbackActive},
            {"playbackStarted", audioStatus.playbackStarted},
            {"hasPlayableAudio", audioStatus.hasPlayableAudio},
            {"clockAvailable", audioStatus.clockAvailable},
            {"outputUnavailable", audioStatus.outputUnavailable},
            {"scheduledSourcePaths", audioStatus.scheduledSourcePaths},
            {"status", audioStatus.message}
        }},
        {"preview", {
            {"generation", previewCompletedGeneration},
            {"success", previewResult.success},
            {"message", previewResult.message},
            {"sourcePath", previewResult.sourcePath},
            {"zeroCopyVulkan", {
                {"ready", previewZeroCopyAvailable},
                {"presentedFrames", previewLastUsedZeroCopy ? previewCompletedGeneration : 0},
                {"failures", previewZeroCopyFailureReason.empty() ? 0 : 1},
                {"failureReason", previewZeroCopyFailureReason},
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
    if (result.applied) {
        shellState->uiPreviewRefreshRequested.store(true, std::memory_order_release);
    }
    return result.applied;
}

ShellLayout computeShellLayout()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float top = viewport->Pos.y + ImGui::GetFrameHeight() + kShellGap;
    const float bottom = viewport->Pos.y + viewport->Size.y - kStatusBarHeight - kShellGap;
    const float left = viewport->Pos.x + kShellGap;
    const float right = viewport->Pos.x + viewport->Size.x - kShellGap;
    const float contentHeight = std::max(260.0f, bottom - top);
    const float contentWidth = std::max(320.0f, right - left);

    if (viewport->Size.x < 760.0f) {
        const float gap = kShellGap * 0.6f;
        const float previewHeight = std::max(170.0f, contentHeight * 0.36f);
        const float timelineHeight = std::max(140.0f, contentHeight * 0.24f);
        const float remainingHeight = std::max(
            120.0f,
            contentHeight - previewHeight - timelineHeight - (3.0f * gap));
        const float secondaryHeight = std::max(96.0f, remainingHeight * 0.5f);
        const float inspectorHeight = std::max(96.0f, remainingHeight - secondaryHeight);

        ShellLayout layout;
        float y = top;
        layout.preview = {{left, y}, {contentWidth, previewHeight}};
        y += previewHeight + gap;
        layout.timeline = {{left, y}, {contentWidth, timelineHeight}};
        y += timelineHeight + gap;
        layout.media = {{left, y}, {contentWidth, secondaryHeight}};
        y += secondaryHeight + gap;
        layout.inspector = {{left, y}, {contentWidth, inspectorHeight}};
        return layout;
    }

    if (viewport->Size.x < 1040.0f) {
        const float gap = kShellGap;
        const float previewHeight = std::max(240.0f, contentHeight * 0.48f);
        const float timelineHeight = std::max(170.0f, contentHeight * 0.25f);
        const float lowerHeight = std::max(150.0f, contentHeight - previewHeight - timelineHeight - (2.0f * gap));
        const float lowerWidth = (contentWidth - gap) * 0.5f;

        ShellLayout layout;
        layout.preview = {{left, top}, {contentWidth, previewHeight}};
        layout.timeline = {{left, top + previewHeight + gap}, {contentWidth, timelineHeight}};
        layout.media = {{left, top + previewHeight + timelineHeight + (2.0f * gap)}, {lowerWidth, lowerHeight}};
        layout.inspector = {{left + lowerWidth + gap, layout.media.pos.y}, {lowerWidth, lowerHeight}};
        return layout;
    }

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

void invalidateProjectHistoryCache(ShellState* shellState)
{
    shellState->projectHistoryEntries.clear();
    shellState->projectHistoryError.clear();
    shellState->projectHistoryRefreshRequested = true;
}

void resetProjectUiState(ShellState* shellState)
{
    ++shellState->documentGeneration;
    shellState->mediaGalleryPath.clear();
    shellState->mediaHoveredPath.clear();
    shellState->mediaSelectedPath.clear();
    shellState->titleDraftClipId = -1;
    shellState->titleDraft = {};
    shellState->keyframeDraft = {};
    shellState->transcriptCache = {};
    shellState->renderSyncMarkerDraft = {};
    shellState->timelineContextClipId = 0;
    shellState->timelineContextClipPersistentId.clear();
    shellState->timelineContextFrame = 0;
    shellState->timelineContextDocumentGeneration = 0;
    invalidateProjectHistoryCache(shellState);
}

jcut::ImGuiProjectSession currentProjectSession(
    const ShellState& shellState,
    const jcut::EditorDocumentCore& document)
{
    jcut::ImGuiProjectSession session;
    session.document = document;
    session.projectId = shellState.projectId;
    session.statePath = shellState.statePath;
    session.historyPath = shellState.historyPath;
    session.rootDirPath = shellState.projectRootPath;
    session.mediaRootPath = shellState.mediaRootDirectory;
    session.legacyStateRoot = shellState.legacyStateRoot;
    session.legacyStateOverrides = shellState.legacyStateOverrides;
    return session;
}

void runAutosaveIfDue(ShellState* shellState,
                      const jcut::EditorDocumentCore& document)
{
    if (!shellState->usesQtProjectStorage ||
        std::chrono::steady_clock::now() < shellState->nextAutosaveAt) {
        return;
    }
    shellState->nextAutosaveAt =
        std::chrono::steady_clock::now() +
        std::chrono::minutes(shellState->autosaveIntervalMinutes);
    std::string backupPath;
    std::string error;
    if (jcut::writeImGuiProjectAutosave(
            currentProjectSession(*shellState, document),
            document,
            shellState->autosaveMaxBackups,
            &backupPath,
            &error)) {
        shellState->statusMessage =
            "autosave backup written: " +
            fs::path(backupPath).filename().string();
    } else {
        shellState->statusMessage = error.empty()
            ? "autosave backup failed"
            : "autosave backup failed: " + error;
    }
}

void loadProjectSessionIntoShell(
    ShellState* shellState,
    const jcut::ImGuiProjectSession& session,
    const std::string& statusMessage)
{
    jcut::EditorDocumentCore loadedDocument = session.document;
    const std::string mediaRoot = session.mediaRootPath.empty()
        ? session.rootDirPath
        : session.mediaRootPath;
    jcut::standalone_render::probeUnknownAudioPresence(
        &loadedDocument, mediaRoot);

    {
        std::lock_guard<std::mutex> lock(shellState->runtimeMutex);
        shellState->runtime = jcut::EditorRuntime::fromDocument(loadedDocument);
    }
    shellState->projectId = session.projectId;
    shellState->statePath = session.statePath;
    shellState->historyPath = session.historyPath;
    shellState->projectRootPath = session.rootDirPath;
    shellState->mediaRootDirectory = mediaRoot;
    std::snprintf(shellState->mediaRootPath.data(),
                  shellState->mediaRootPath.size(),
                  "%s",
                  shellState->mediaRootDirectory.c_str());
    shellState->legacyStateRoot = session.legacyStateRoot;
    shellState->legacyStateOverrides = nlohmann::json::object();
    reloadProjectPreferenceState(shellState);
    shellState->usesQtProjectStorage = true;
    shellState->lastSavedSnapshotJson = snapshotJson(loadedDocument);
    shellState->lastSavedLegacyExtensionSignature =
        legacyExtensionSignature(*shellState);
    std::snprintf(shellState->exportOutputPath.data(),
                  shellState->exportOutputPath.size(),
                  "%s",
                  loadedDocument.exportRequest.outputPath.c_str());
    resetProjectUiState(shellState);
    shellState->statusMessage = statusMessage;
    requestPreviewRender(shellState);
}

bool saveCurrentDocument(ShellState* shellState)
{
    if (shellState->documentPath.empty() && !shellState->usesQtProjectStorage) {
        shellState->statusMessage = "save unavailable: no document path";
        return false;
    }
    if (!commitExportOutputPathDraft(shellState)) {
        return false;
    }

    jcut::EditorDocumentCore snapshot;
    {
        std::lock_guard<std::mutex> lock(shellState->runtimeMutex);
        snapshot = shellState->runtime.snapshot();
    }
    if (shellState->usesQtProjectStorage) {
        const jcut::ImGuiProjectSession session =
            currentProjectSession(*shellState, snapshot);
        std::string error;
        if (!jcut::saveImGuiProjectSession(session, snapshot, &error)) {
            shellState->statusMessage = error;
            return false;
        }
        commitLegacyStateOverrides(shellState, snapshot);
        shellState->lastSavedSnapshotJson = snapshotJson(snapshot);
        invalidateProjectHistoryCache(shellState);
        shellState->statusMessage = "project state saved";
        return true;
    }

    std::string error;
    if (!jcut::saveEditorDocumentCoreToFile(snapshot, shellState->documentPath, &error)) {
        shellState->statusMessage = error;
        return false;
    }

    shellState->lastSavedSnapshotJson = snapshotJson(snapshot);
    shellState->lastSavedLegacyExtensionSignature =
        legacyExtensionSignature(*shellState);
    shellState->statusMessage = "document saved";
    return true;
}

bool reloadCurrentDocument(ShellState* shellState)
{
    if (shellState->documentPath.empty() && !shellState->usesQtProjectStorage) {
        shellState->statusMessage = "reload unavailable: no document path";
        return false;
    }
    if (!commitExportOutputPathDraft(shellState)) {
        return false;
    }
    jcut::EditorDocumentCore currentDocument;
    {
        std::lock_guard<std::mutex> lock(shellState->runtimeMutex);
        currentDocument = shellState->runtime.snapshot();
    }
    if (documentIsDirty(*shellState, currentDocument)) {
        shellState->statusMessage = "reload blocked: save changes before reloading";
        return false;
    }

    if (shellState->usesQtProjectStorage) {
        std::string error;
        const std::optional<jcut::ImGuiProjectSession> session =
            jcut::loadActiveImGuiProjectSession(&error);
        if (!session.has_value()) {
            shellState->statusMessage = error;
            return false;
        }

        loadProjectSessionIntoShell(
            shellState, *session, "active project reloaded");
        return true;
    }

    std::string error;
    const std::optional<jcut::EditorDocumentCore> loadedDocument =
        jcut::loadEditorDocumentCoreFromFile(shellState->documentPath, &error);
    if (!loadedDocument.has_value()) {
        shellState->statusMessage = error;
        return false;
    }

    shellState->projectRootPath = pathString(fs::path(shellState->documentPath).parent_path());
    shellState->mediaRootDirectory = shellState->projectRootPath;
    jcut::EditorDocumentCore document = *loadedDocument;
    jcut::standalone_render::probeUnknownAudioPresence(
        &document, shellState->mediaRootDirectory);

    {
        std::lock_guard<std::mutex> lock(shellState->runtimeMutex);
        shellState->runtime = jcut::EditorRuntime::fromDocument(document);
    }
    shellState->lastSavedSnapshotJson = snapshotJson(document);
    std::snprintf(shellState->mediaRootPath.data(),
                  shellState->mediaRootPath.size(),
                  "%s",
                  shellState->mediaRootDirectory.c_str());
    std::snprintf(shellState->exportOutputPath.data(),
                  shellState->exportOutputPath.size(),
                  "%s",
                  document.exportRequest.outputPath.c_str());
    resetProjectUiState(shellState);
    shellState->statusMessage = "document reloaded";
    requestPreviewRender(shellState);
    return true;
}

bool documentIsDirty(const ShellState& shellState, const jcut::EditorDocumentCore& snapshot)
{
    if (shellState.documentPath.empty() && !shellState.usesQtProjectStorage) {
        return false;
    }
    return snapshotJson(snapshot) != shellState.lastSavedSnapshotJson ||
        (shellState.usesQtProjectStorage &&
         legacyExtensionSignature(shellState) !=
             shellState.lastSavedLegacyExtensionSignature);
}

void adoptSavedProjectSession(
    ShellState* shellState,
    const jcut::ImGuiProjectSession& session,
    const jcut::EditorDocumentCore& savedDocument,
    const std::string& statusMessage)
{
    shellState->projectId = session.projectId;
    shellState->statePath = session.statePath;
    shellState->historyPath = session.historyPath;
    shellState->projectRootPath = session.rootDirPath;
    shellState->mediaRootDirectory = session.mediaRootPath.empty()
        ? session.rootDirPath
        : session.mediaRootPath;
    std::snprintf(shellState->mediaRootPath.data(),
                  shellState->mediaRootPath.size(),
                  "%s",
                  shellState->mediaRootDirectory.c_str());
    shellState->legacyStateRoot = session.legacyStateRoot;
    shellState->legacyStateOverrides = nlohmann::json::object();
    shellState->usesQtProjectStorage = true;
    shellState->lastSavedSnapshotJson = snapshotJson(savedDocument);
    shellState->lastSavedLegacyExtensionSignature =
        legacyExtensionSignature(*shellState);
    invalidateProjectHistoryCache(shellState);
    shellState->statusMessage = statusMessage;
}

const char* projectLifecycleTitle(ProjectLifecycleAction action)
{
    switch (action) {
    case ProjectLifecycleAction::NewProject:
        return "New Project";
    case ProjectLifecycleAction::SaveAs:
        return "Save Project As";
    case ProjectLifecycleAction::Rename:
        return "Rename Project";
    case ProjectLifecycleAction::None:
        break;
    }
    return "Project";
}

void requestProjectLifecycleAction(
    ShellState* shellState,
    ProjectLifecycleAction action,
    const jcut::EditorDocumentCore& snapshot)
{
    if (!shellState->usesQtProjectStorage) {
        shellState->statusMessage =
            "project lifecycle actions require Qt project storage";
        return;
    }
    if ((action == ProjectLifecycleAction::NewProject ||
         action == ProjectLifecycleAction::Rename) &&
        documentIsDirty(*shellState, snapshot)) {
        shellState->statusMessage =
            "save changes before creating or renaming a project";
        return;
    }

    const char* initialName = "Untitled Project";
    if (action == ProjectLifecycleAction::SaveAs ||
        action == ProjectLifecycleAction::Rename) {
        initialName = shellState->projectId.empty()
            ? snapshot.projectName.c_str()
            : shellState->projectId.c_str();
    }
    std::snprintf(shellState->projectNameDraft.data(),
                  shellState->projectNameDraft.size(),
                  "%s",
                  initialName);
    shellState->projectLifecycleAction = action;
    shellState->projectLifecyclePopupRequested = true;
}

bool performProjectLifecycleAction(ShellState* shellState)
{
    if (!commitExportOutputPathDraft(shellState)) {
        return false;
    }
    const jcut::EditorDocumentCore snapshot = runtimeSnapshot(shellState);
    const ProjectLifecycleAction action = shellState->projectLifecycleAction;
    if ((action == ProjectLifecycleAction::NewProject ||
         action == ProjectLifecycleAction::Rename) &&
        documentIsDirty(*shellState, snapshot)) {
        shellState->statusMessage =
            "save changes before creating or renaming a project";
        return false;
    }

    std::string error;
    std::optional<jcut::ImGuiProjectSession> resultingSession;
    switch (action) {
    case ProjectLifecycleAction::NewProject:
        resultingSession = jcut::createImGuiProjectSession(
            shellState->projectNameDraft.data(), &error);
        break;
    case ProjectLifecycleAction::SaveAs:
        resultingSession = jcut::saveImGuiProjectSessionAs(
            currentProjectSession(*shellState, snapshot),
            snapshot,
            shellState->projectNameDraft.data(),
            &error);
        break;
    case ProjectLifecycleAction::Rename:
        resultingSession = jcut::renameImGuiProjectSession(
            currentProjectSession(*shellState, snapshot),
            shellState->projectNameDraft.data(),
            &error);
        break;
    case ProjectLifecycleAction::None:
        shellState->statusMessage = "no project action selected";
        return false;
    }
    if (!resultingSession.has_value()) {
        shellState->statusMessage = error.empty()
            ? "project action failed"
            : error;
        return false;
    }

    const std::string projectId = resultingSession->projectId;
    if (action == ProjectLifecycleAction::NewProject) {
        loadProjectSessionIntoShell(
            shellState,
            *resultingSession,
            "new project created: " + projectId);
    } else {
        adoptSavedProjectSession(
            shellState,
            *resultingSession,
            snapshot,
            action == ProjectLifecycleAction::SaveAs
                ? "project saved as: " + projectId
                : "project renamed: " + projectId);
    }
    return true;
}

void drawProjectLifecyclePopup(ShellState* shellState)
{
    if (shellState->projectLifecyclePopupRequested) {
        ImGui::OpenPopup("Project Lifecycle");
        shellState->projectLifecyclePopupRequested = false;
    }
    if (!ImGui::BeginPopupModal(
            "Project Lifecycle", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    ImGui::TextUnformatted(projectLifecycleTitle(
        shellState->projectLifecycleAction));
    const bool submitFromKeyboard = ImGui::InputText(
        "Project name",
        shellState->projectNameDraft.data(),
        shellState->projectNameDraft.size(),
        ImGuiInputTextFlags_EnterReturnsTrue);
    const char* submitLabel = shellState->projectLifecycleAction ==
            ProjectLifecycleAction::SaveAs
        ? "Save As"
        : (shellState->projectLifecycleAction == ProjectLifecycleAction::Rename
               ? "Rename"
               : "Create");
    if (ImGui::Button(submitLabel) || submitFromKeyboard) {
        if (performProjectLifecycleAction(shellState)) {
            shellState->projectLifecycleAction = ProjectLifecycleAction::None;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        shellState->projectLifecycleAction = ProjectLifecycleAction::None;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void runPreviewWorker(ShellState* shellState)
{
    for (;;) {
        jcut::EditorDocumentCore document;
        std::string rootDirectory;
        std::uint64_t generation = 0;
        bool preferVulkanFrame = true;
        {
            std::unique_lock<std::mutex> lock(shellState->previewMutex);
            shellState->previewCondition.wait(lock, [shellState]() {
                return shellState->previewStopRequested ||
                    (shellState->previewRenderRequested &&
                     shellState->previewUploadedGeneration >=
                         shellState->previewCompletedGeneration);
            });
            if (shellState->previewStopRequested) {
                return;
            }
            document = shellState->previewDocument;
            rootDirectory = shellState->previewRootDirectory;
            generation = shellState->previewRequestGeneration;
            preferVulkanFrame = !shellState->previewCpuFallbackPreferred &&
                !document.panels.showScopes;
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
                    preferVulkanFrame,
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

int adjacentOrdinaryTrackIndex(const jcut::EditorDocumentCore& snapshot,
                               int trackIndex,
                               int direction)
{
    if (direction == 0) {
        return -1;
    }
    for (int candidate = trackIndex + (direction < 0 ? -1 : 1);
         candidate >= 0 &&
         candidate < static_cast<int>(snapshot.tracks.size());
         candidate += direction < 0 ? -1 : 1) {
        if (!jcut::isGeneratedEditorChildTrack(
                snapshot.tracks[static_cast<std::size_t>(candidate)])) {
            return candidate;
        }
    }
    return -1;
}

struct TimelineTrackDropTarget {
    int trackIndex = -1;
    bool insertTrack = false;
};

TimelineTrackDropTarget timelineTrackDropTarget(
    const jcut::EditorDocumentCore& snapshot,
    float originY,
    float mouseY)
{
    if (snapshot.tracks.empty()) {
        return {0, true};
    }
    const float relative = mouseY - (originY + kTimelineTopPadding);
    if (relative < 0.0f) {
        return {0, true};
    }
    const int row = static_cast<int>(std::floor(relative / kTimelineRowHeight));
    if (row >= static_cast<int>(snapshot.tracks.size())) {
        return {static_cast<int>(snapshot.tracks.size()), true};
    }
    if (jcut::isGeneratedEditorChildTrack(
            snapshot.tracks[static_cast<std::size_t>(row)])) {
        // Generated Mask Matte lanes are derived. Match the Qt drop policy by
        // inserting a normal lane before the child instead of targeting it.
        return {row, true};
    }
    const float withinRow = relative - static_cast<float>(row) * kTimelineRowHeight;
    if (withinRow < kTimelineClipHeight) {
        return {row, false};
    }
    return {row + 1, true};
}

template <typename InsertCommand>
void insertDroppedMedia(ShellState* shellState,
                        const jcut::EditorDocumentCore& snapshot,
                        const TimelineTrackDropTarget& requestedTarget,
                        const std::string& mediaKind,
                        int startFrame,
                        int durationFrames,
                        InsertCommand&& insertCommand)
{
    int targetTrackIndex = requestedTarget.trackIndex;
    bool createTrack = requestedTarget.insertTrack || snapshot.tracks.empty();
    if (!createTrack) {
        targetTrackIndex = jcut::firstNonConflictingTrackIndex(
            snapshot,
            requestedTarget.trackIndex,
            mediaKind,
            startFrame,
            durationFrames);
        if (targetTrackIndex >= 0 &&
            jcut::isGeneratedEditorChildTrack(snapshot.tracks[
                static_cast<std::size_t>(targetTrackIndex)])) {
            targetTrackIndex = -1;
        }
        createTrack = targetTrackIndex < 0;
    }

    const bool startedTransaction = createTrack &&
        !shellState->historyTransactionActive;
    if (startedTransaction) {
        beginRuntimeHistoryTransaction(shellState);
    }

    int targetTrackId = 0;
    if (createTrack) {
        const int requestedInsertionIndex = std::clamp(
            requestedTarget.insertTrack
                ? requestedTarget.trackIndex
                : static_cast<int>(snapshot.tracks.size()),
            0,
            static_cast<int>(snapshot.tracks.size()));
        applyCommand(shellState, jcut::AddTrackCommand{
            mediaKind == "audio" ? "Audio" : "Video",
            requestedInsertionIndex});
        const jcut::EditorDocumentCore afterAdd = runtimeSnapshot(shellState);
        if (afterAdd.tracks.size() > snapshot.tracks.size()) {
            const auto insertedTrack = std::find_if(
                afterAdd.tracks.begin(), afterAdd.tracks.end(),
                [&](const jcut::EditorTrack& candidate) {
                    return candidate.selected &&
                        std::none_of(
                            snapshot.tracks.begin(), snapshot.tracks.end(),
                            [&](const jcut::EditorTrack& previous) {
                                return previous.id == candidate.id;
                            });
                });
            if (insertedTrack != afterAdd.tracks.end()) {
                targetTrackId = insertedTrack->id;
            }
        }
    } else if (targetTrackIndex >= 0 &&
               targetTrackIndex < static_cast<int>(snapshot.tracks.size())) {
        targetTrackId = snapshot.tracks[static_cast<std::size_t>(
            targetTrackIndex)].id;
    }

    if (targetTrackId > 0) {
        std::forward<InsertCommand>(insertCommand)(targetTrackId);
    } else {
        shellState->statusMessage = "unable to create a timeline track for media";
    }

    if (startedTransaction) {
        endRuntimeHistoryTransaction(shellState);
    }
}

struct TimelineSnapResult {
    int frame = 0;
    int boundaryFrame = -1;
};

int timelineSnapThresholdFrames()
{
    return std::max(1, static_cast<int>(std::lround(
        kTimelineSnapThresholdPixels / std::max(0.25f, kTimelinePixelsPerFrame))));
}

TimelineSnapResult snapTimelineBoundary(const jcut::EditorDocumentCore& snapshot,
                                        int proposedFrame,
                                        int excludedClipId = 0,
                                        bool excludeSelected = false)
{
    const std::int64_t proposed = std::max(0, proposedFrame);
    const std::int64_t threshold = timelineSnapThresholdFrames();
    std::int64_t bestFrame = proposed;
    std::int64_t bestDistance = threshold + 1;
    auto consider = [&](std::int64_t candidate) {
        const std::int64_t distance = candidate >= proposed
            ? candidate - proposed
            : proposed - candidate;
        if (distance <= threshold && distance < bestDistance) {
            bestDistance = distance;
            bestFrame = candidate;
        }
    };

    consider(0);
    for (const jcut::EditorClip& clip : snapshot.clips) {
        if (clip.id == excludedClipId || (excludeSelected && clip.selected)) {
            continue;
        }
        consider(clip.startFrame);
        consider(static_cast<std::int64_t>(clip.startFrame) + clip.durationFrames);
    }

    return {
        static_cast<int>(std::clamp<std::int64_t>(
            bestFrame, 0, std::numeric_limits<int>::max())),
        bestDistance <= threshold
            ? static_cast<int>(std::clamp<std::int64_t>(
                  bestFrame, 0, std::numeric_limits<int>::max()))
            : -1};
}

TimelineSnapResult snapTimelineMoveStart(const jcut::EditorDocumentCore& snapshot,
                                         int anchorClipId,
                                         int proposedStartFrame)
{
    const auto anchorIt = std::find_if(
        snapshot.clips.begin(), snapshot.clips.end(),
        [&](const jcut::EditorClip& clip) { return clip.id == anchorClipId; });
    if (anchorIt == snapshot.clips.end()) {
        return {std::max(0, proposedStartFrame), -1};
    }

    const std::int64_t proposedStart = std::max(0, proposedStartFrame);
    const std::int64_t proposedEnd = proposedStart + anchorIt->durationFrames;
    const std::int64_t threshold = timelineSnapThresholdFrames();
    std::int64_t bestStart = proposedStart;
    std::int64_t bestBoundary = -1;
    std::int64_t bestDistance = threshold + 1;
    auto consider = [&](std::int64_t boundary) {
        const std::int64_t startDistance = boundary >= proposedStart
            ? boundary - proposedStart
            : proposedStart - boundary;
        if (startDistance <= threshold && startDistance < bestDistance) {
            bestDistance = startDistance;
            bestStart = boundary;
            bestBoundary = boundary;
        }
        const std::int64_t endDistance = boundary >= proposedEnd
            ? boundary - proposedEnd
            : proposedEnd - boundary;
        if (endDistance <= threshold && endDistance < bestDistance) {
            bestDistance = endDistance;
            bestStart = std::max<std::int64_t>(0, boundary - anchorIt->durationFrames);
            bestBoundary = boundary;
        }
    };

    consider(0);
    for (const jcut::EditorClip& clip : snapshot.clips) {
        // All selected clips move as one aggregate. Their internal boundaries
        // must not attract the group back toward its previous position.
        if (clip.selected) {
            continue;
        }
        consider(clip.startFrame);
        consider(static_cast<std::int64_t>(clip.startFrame) + clip.durationFrames);
    }

    return {
        static_cast<int>(std::clamp<std::int64_t>(
            bestStart, 0, std::numeric_limits<int>::max())),
        bestDistance <= threshold
            ? static_cast<int>(std::clamp<std::int64_t>(
                  bestBoundary, 0, std::numeric_limits<int>::max()))
            : -1};
}

void clearTimelineDrag(ShellState* shellState)
{
    shellState->timelineDragMode = TimelineDragMode::None;
    shellState->timelineDragClipId = 0;
    shellState->timelineDragTrackId = 0;
    shellState->timelineDragTrackIndex = -1;
    shellState->timelineSnapIndicatorFrame = -1;
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
    case XK_minus:
    case XK_underscore: return ImGuiKey_Minus;
    case XK_period: return ImGuiKey_Period;
    case XK_slash: return ImGuiKey_Slash;
    case XK_semicolon: return ImGuiKey_Semicolon;
    case XK_equal:
    case XK_plus: return ImGuiKey_Equal;
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

    void updateModifiers(unsigned int state, KeySym keySym, bool down)
    {
        ImGuiIO& io = ImGui::GetIO();
        bool ctrl = (state & ControlMask) != 0;
        bool shift = (state & ShiftMask) != 0;
        bool alt = (state & Mod1Mask) != 0;
        bool super = (state & Mod4Mask) != 0;
        if (keySym == XK_Control_L || keySym == XK_Control_R) ctrl = down;
        if (keySym == XK_Shift_L || keySym == XK_Shift_R) shift = down;
        if (keySym == XK_Alt_L || keySym == XK_Alt_R) alt = down;
        if (keySym == XK_Super_L || keySym == XK_Super_R) super = down;
        io.AddKeyEvent(ImGuiMod_Ctrl, ctrl);
        io.AddKeyEvent(ImGuiMod_Shift, shift);
        io.AddKeyEvent(ImGuiMod_Alt, alt);
        io.AddKeyEvent(ImGuiMod_Super, super);
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
                updateModifiers(event.xkey.state, keySym, down);
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
    VkCommandPool uploadCommandPool = VK_NULL_HANDLE;
    VkImage previewImage = VK_NULL_HANDLE;
    VkDeviceMemory previewImageMemory = VK_NULL_HANDLE;
    VkImageView previewImageView = VK_NULL_HANDLE;
    jcut::core::SizeI previewImageSize{};
    VkImageLayout previewImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ImGui_ImplVulkanH_Window windowData{};
    uint32_t minImageCount = 2;
    bool swapchainRebuild = false;
    jcut::imgui::VulkanFrameImporter previewHandoff;
    bool previewHandoffInitialized = false;
    std::string previewHandoffStatus;
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

    bool createUploadCommandPool(std::string* error)
    {
        VkCommandPoolCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        info.queueFamilyIndex = queueFamily;
        if (vkCreateCommandPool(device, &info, nullptr, &uploadCommandPool) != VK_SUCCESS) {
            if (error) *error = "Failed to create Vulkan preview upload command pool.";
            return false;
        }
        return true;
    }

    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties) const
    {
        VkPhysicalDeviceMemoryProperties memoryProperties{};
        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);
        for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
            if ((typeBits & (1u << i)) &&
                (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties) {
                return i;
            }
        }
        return UINT32_MAX;
    }

    bool createBuffer(VkDeviceSize size,
                      VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags properties,
                      VkBuffer* buffer,
                      VkDeviceMemory* memory,
                      std::string* error)
    {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = size;
        bufferInfo.usage = usage;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device, &bufferInfo, nullptr, buffer) != VK_SUCCESS) {
            if (error) *error = "Failed to create Vulkan preview upload buffer.";
            return false;
        }

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, *buffer, &requirements);
        const uint32_t memoryType = findMemoryType(requirements.memoryTypeBits, properties);
        if (memoryType == UINT32_MAX) {
            if (error) *error = "No suitable memory type for Vulkan preview upload buffer.";
            vkDestroyBuffer(device, *buffer, nullptr);
            *buffer = VK_NULL_HANDLE;
            return false;
        }

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = requirements.size;
        allocInfo.memoryTypeIndex = memoryType;
        if (vkAllocateMemory(device, &allocInfo, nullptr, memory) != VK_SUCCESS) {
            if (error) *error = "Failed to allocate Vulkan preview upload buffer memory.";
            vkDestroyBuffer(device, *buffer, nullptr);
            *buffer = VK_NULL_HANDLE;
            return false;
        }
        vkBindBufferMemory(device, *buffer, *memory, 0);
        return true;
    }

    bool createPreviewImage(const jcut::core::SizeI& size, std::string* error)
    {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent = {
            static_cast<uint32_t>(size.width),
            static_cast<uint32_t>(size.height),
            1
        };
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateImage(device, &imageInfo, nullptr, &previewImage) != VK_SUCCESS) {
            if (error) *error = "Failed to create Vulkan preview image.";
            return false;
        }

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device, previewImage, &requirements);
        const uint32_t memoryType =
            findMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (memoryType == UINT32_MAX) {
            if (error) *error = "No suitable memory type for Vulkan preview image.";
            releasePreviewTextureResources();
            return false;
        }

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = requirements.size;
        allocInfo.memoryTypeIndex = memoryType;
        if (vkAllocateMemory(device, &allocInfo, nullptr, &previewImageMemory) != VK_SUCCESS) {
            if (error) *error = "Failed to allocate Vulkan preview image memory.";
            releasePreviewTextureResources();
            return false;
        }
        vkBindImageMemory(device, previewImage, previewImageMemory, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = previewImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;
        if (vkCreateImageView(device, &viewInfo, nullptr, &previewImageView) != VK_SUCCESS) {
            if (error) *error = "Failed to create Vulkan preview image view.";
            releasePreviewTextureResources();
            return false;
        }

        previewImageSize = size;
        previewImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        return true;
    }

    VkCommandBuffer beginUploadCommands(std::string* error)
    {
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = uploadCommandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer) != VK_SUCCESS) {
            if (error) *error = "Failed to allocate Vulkan preview upload command buffer.";
            return VK_NULL_HANDLE;
        }

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
            if (error) *error = "Failed to begin Vulkan preview upload command buffer.";
            vkFreeCommandBuffers(device, uploadCommandPool, 1, &commandBuffer);
            return VK_NULL_HANDLE;
        }
        return commandBuffer;
    }

    bool endUploadCommands(VkCommandBuffer commandBuffer, std::string* error)
    {
        if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
            if (error) *error = "Failed to finish Vulkan preview upload command buffer.";
            vkFreeCommandBuffers(device, uploadCommandPool, 1, &commandBuffer);
            return false;
        }
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;
        if (vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS ||
            vkQueueWaitIdle(queue) != VK_SUCCESS) {
            if (error) *error = "Failed to submit Vulkan preview upload commands.";
            vkFreeCommandBuffers(device, uploadCommandPool, 1, &commandBuffer);
            return false;
        }
        vkFreeCommandBuffers(device, uploadCommandPool, 1, &commandBuffer);
        return true;
    }

    void transitionPreviewImage(VkCommandBuffer commandBuffer,
                                VkImageLayout oldLayout,
                                VkImageLayout newLayout)
    {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = previewImage;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;

        VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        if (oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            srcStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        }
        if (newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        } else if (newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        }

        vkCmdPipelineBarrier(commandBuffer,
                             srcStage,
                             dstStage,
                             0,
                             0,
                             nullptr,
                             0,
                             nullptr,
                             1,
                             &barrier);
    }

    void releasePreviewTextureResources()
    {
        if (device == VK_NULL_HANDLE) {
            return;
        }
        if (previewTextureSet != VK_NULL_HANDLE) {
            ImGui_ImplVulkan_RemoveTexture(previewTextureSet);
            previewTextureSet = VK_NULL_HANDLE;
        }
        if (previewImageView != VK_NULL_HANDLE) {
            vkDestroyImageView(device, previewImageView, nullptr);
            previewImageView = VK_NULL_HANDLE;
        }
        if (previewImage != VK_NULL_HANDLE) {
            vkDestroyImage(device, previewImage, nullptr);
            previewImage = VK_NULL_HANDLE;
        }
        if (previewImageMemory != VK_NULL_HANDLE) {
            vkFreeMemory(device, previewImageMemory, nullptr);
            previewImageMemory = VK_NULL_HANDLE;
        }
        previewImageSize = {};
        previewImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        boundPreviewView = VK_NULL_HANDLE;
        boundPreviewLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    bool uploadPreviewImage(const jcut::core::ImageBuffer& image,
                            ImTextureID* textureOut,
                            std::string* error)
    {
        if (image.empty() || image.format != jcut::core::PixelFormat::Rgba8) {
            if (error) *error = "Invalid CPU preview image.";
            return false;
        }
        const VkDeviceSize byteCount =
            static_cast<VkDeviceSize>(image.size.width) *
            static_cast<VkDeviceSize>(image.size.height) * 4;
        std::vector<std::uint8_t> packed;
        const std::uint8_t* uploadBytes = image.bytes.data();
        if (image.strideBytes != image.size.width * 4) {
            packed.resize(static_cast<std::size_t>(byteCount));
            for (int y = 0; y < image.size.height; ++y) {
                std::memcpy(packed.data() + static_cast<std::size_t>(y * image.size.width * 4),
                            image.bytes.data() + static_cast<std::size_t>(y * image.strideBytes),
                            static_cast<std::size_t>(image.size.width * 4));
            }
            uploadBytes = packed.data();
        }

        VkBuffer stagingBuffer = VK_NULL_HANDLE;
        VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
        if (!createBuffer(byteCount,
                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          &stagingBuffer,
                          &stagingMemory,
                          error)) {
            return false;
        }

        void* mapped = nullptr;
        if (vkMapMemory(device, stagingMemory, 0, byteCount, 0, &mapped) != VK_SUCCESS || !mapped) {
            if (error) *error = "Failed to map Vulkan preview upload memory.";
            vkDestroyBuffer(device, stagingBuffer, nullptr);
            vkFreeMemory(device, stagingMemory, nullptr);
            return false;
        }
        std::memcpy(mapped, uploadBytes, static_cast<std::size_t>(byteCount));
        vkUnmapMemory(device, stagingMemory);

        if (previewImage == VK_NULL_HANDLE ||
            previewImageSize.width != image.size.width ||
            previewImageSize.height != image.size.height) {
            vkDeviceWaitIdle(device);
            releasePreviewTextureResources();
            if (!createPreviewImage(image.size, error)) {
                vkDestroyBuffer(device, stagingBuffer, nullptr);
                vkFreeMemory(device, stagingMemory, nullptr);
                return false;
            }
        }

        VkCommandBuffer commandBuffer = beginUploadCommands(error);
        if (commandBuffer == VK_NULL_HANDLE) {
            vkDestroyBuffer(device, stagingBuffer, nullptr);
            vkFreeMemory(device, stagingMemory, nullptr);
            return false;
        }
        transitionPreviewImage(commandBuffer, previewImageLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        VkBufferImageCopy copyRegion{};
        copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.imageSubresource.mipLevel = 0;
        copyRegion.imageSubresource.baseArrayLayer = 0;
        copyRegion.imageSubresource.layerCount = 1;
        copyRegion.imageExtent = {
            static_cast<uint32_t>(image.size.width),
            static_cast<uint32_t>(image.size.height),
            1
        };
        vkCmdCopyBufferToImage(commandBuffer,
                               stagingBuffer,
                               previewImage,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1,
                               &copyRegion);
        transitionPreviewImage(commandBuffer,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        const bool submitted = endUploadCommands(commandBuffer, error);
        vkDestroyBuffer(device, stagingBuffer, nullptr);
        vkFreeMemory(device, stagingMemory, nullptr);
        if (!submitted) {
            return false;
        }
        previewImageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        if (previewTextureSet == VK_NULL_HANDLE ||
            boundPreviewView != previewImageView ||
            boundPreviewLayout != previewImageLayout) {
            if (previewTextureSet != VK_NULL_HANDLE) {
                ImGui_ImplVulkan_RemoveTexture(previewTextureSet);
            }
            previewTextureSet =
                ImGui_ImplVulkan_AddTexture(previewSampler, previewImageView, previewImageLayout);
            boundPreviewView = previewImageView;
            boundPreviewLayout = previewImageLayout;
        }
        if (textureOut) {
            *textureOut = reinterpret_cast<ImTextureID>(previewTextureSet);
        }
        return previewTextureSet != VK_NULL_HANDLE;
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
            !createPreviewSampler(error) ||
            !createUploadCommandPool(error)) {
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
        previewHandoffInitialized = previewHandoff.initialize(
            physicalDevice, device, queue, queueFamily, &handoffError);
        previewHandoffStatus = previewHandoffInitialized
            ? std::string("ready")
            : (handoffError.empty()
                ? std::string("Vulkan external frame import unavailable")
                : handoffError);
        return true;
    }

    bool bindPreviewFrame(const render_detail::OffscreenVulkanFrame& frame,
                          ImTextureID* textureOut,
                          std::string* error)
    {
        if (!frame.valid) {
            if (error) *error = "invalid offscreen Vulkan frame";
            return false;
        }
        if (!previewHandoffInitialized) {
            if (error) *error = previewHandoffStatus.empty()
                ? "Vulkan external frame import unavailable"
                : previewHandoffStatus;
            return false;
        }
        if (!previewHandoff.importFrame(frame, error)) {
            return false;
        }
        const jcut::imgui::VulkanExternalImage external =
            previewHandoff.externalImage();
        if (external.imageView == VK_NULL_HANDLE || !external.size.valid()) {
            if (error) *error = "imported Vulkan preview image is unavailable";
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
        releasePreviewTextureResources();
        previewHandoff.release();
        previewHandoffInitialized = false;
        ImGui_ImplVulkan_Shutdown();
        if (device != VK_NULL_HANDLE && windowData.Surface != VK_NULL_HANDLE) {
            ImGui_ImplVulkanH_DestroyWindow(instance, device, &windowData, nullptr);
        }
        if (previewSampler != VK_NULL_HANDLE) {
            vkDestroySampler(device, previewSampler, nullptr);
            previewSampler = VK_NULL_HANDLE;
        }
        if (uploadCommandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device, uploadCommandPool, nullptr);
            uploadCommandPool = VK_NULL_HANDLE;
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

    bool uploadedTexture = false;
    bool usedZeroCopy = false;
    bool zeroCopyAvailable = false;
    std::string zeroCopyFailureReason;
    bool requestCpuFallbackFrame = false;
    if (vulkanShell && previewResult.vulkanFrame.valid) {
        ImTextureID texture = 0;
        std::string error;
        if (vulkanShell->bindPreviewFrame(previewResult.vulkanFrame, &texture, &error)) {
            shellState->previewTextureId = texture;
            uploadedTexture = true;
            usedZeroCopy = true;
            zeroCopyAvailable = true;
        } else {
            zeroCopyFailureReason = error.empty()
                ? "Vulkan external frame import failed"
                : error;
            if (previewResult.image.empty()) {
                requestCpuFallbackFrame = true;
            }
        }
    } else if (!previewResult.vulkanFrame.valid) {
        zeroCopyFailureReason = "preview renderer did not return a Vulkan frame";
    }

    if (!usedZeroCopy && vulkanShell && !previewResult.image.empty()) {
        ImTextureID texture = 0;
        std::string error;
        if (vulkanShell->uploadPreviewImage(previewResult.image, &texture, &error)) {
            shellState->previewTextureId = texture;
            uploadedTexture = true;
        } else if (!error.empty()) {
            std::fprintf(stderr, "Vulkan CPU preview upload failed: %s\n", error.c_str());
            if (zeroCopyFailureReason.empty()) {
                zeroCopyFailureReason = error;
            }
        }
    }

    if (!uploadedTexture) {
        shellState->previewTextureId = 0;
    }

    {
        std::lock_guard<std::mutex> lock(shellState->previewMutex);
        shellState->previewUploadedGeneration = completedGeneration;
        shellState->previewLastUsedZeroCopy = usedZeroCopy;
        shellState->previewZeroCopyAvailable = zeroCopyAvailable;
        shellState->previewZeroCopyFailureReason = usedZeroCopy ? std::string{} : zeroCopyFailureReason;
        if (requestCpuFallbackFrame) {
            shellState->previewCpuFallbackPreferred = true;
        } else if (usedZeroCopy) {
            shellState->previewCpuFallbackPreferred = false;
        }
    }
    shellState->previewCondition.notify_one();
    if (requestCpuFallbackFrame) {
        requestPreviewRender(shellState);
    }
}

void handleKeyboardShortcuts(ShellState* shellState,
                             const jcut::EditorDocumentCore& snapshot)
{
    const auto repeatingShortcut = [](ImGuiKey key, ImGuiKeyChord modifiers) {
        return ImGui::GetIO().KeyMods == modifiers && ImGui::IsKeyPressed(key, true);
    };

    const bool increaseFont =
        repeatingShortcut(ImGuiKey_Equal, ImGuiMod_Ctrl) ||
        repeatingShortcut(ImGuiKey_Equal, ImGuiMod_Ctrl | ImGuiMod_Shift) ||
        repeatingShortcut(ImGuiKey_KeypadAdd, ImGuiMod_Ctrl);
    const bool decreaseFont =
        repeatingShortcut(ImGuiKey_Minus, ImGuiMod_Ctrl) ||
        repeatingShortcut(ImGuiKey_Minus, ImGuiMod_Ctrl | ImGuiMod_Shift) ||
        repeatingShortcut(ImGuiKey_KeypadSubtract, ImGuiMod_Ctrl);
    if (increaseFont) {
        changeUiFontSize(shellState, 1.0f);
    } else if (decreaseFont) {
        changeUiFontSize(shellState, -1.0f);
    }

    if (ImGui::GetIO().WantTextInput || ImGui::IsAnyItemActive()) {
        return;
    }

    if (ImGui::IsKeyChordPressed(
            ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S)) {
        requestProjectLifecycleAction(
            shellState, ProjectLifecycleAction::SaveAs, snapshot);
    } else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S)) {
        saveCurrentDocument(shellState);
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_N)) {
        requestProjectLifecycleAction(
            shellState, ProjectLifecycleAction::NewProject, snapshot);
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_R)) {
        reloadCurrentDocument(shellState);
    }

    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)) {
        applyCommand(shellState, jcut::UndoCommand{});
    } else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z) ||
               ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y)) {
        applyCommand(shellState, jcut::RedoCommand{});
    }

    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_A)) {
        applyCommand(shellState, jcut::SelectAllClipsCommand{});
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C)) {
        applyCommand(shellState, jcut::CopySelectedClipsCommand{});
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_X)) {
        applyCommand(shellState, jcut::CutSelectedClipsCommand{});
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_V)) {
        applyCommand(shellState, jcut::PasteClipsCommand{
            snapshot.transport.currentFrame, selectedTrackId(snapshot)});
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_D)) {
        applyCommand(shellState, jcut::DuplicateSelectedClipsCommand{});
    }

    const jcut::EditorClip* currentClip = selectedClip(snapshot);
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_B)) {
        if (currentClip) {
            applyCommand(shellState, jcut::SplitClipCommand{
                currentClip->id,
                snapshot.transport.currentFrame});
        } else {
            shellState->statusMessage = "split unavailable: no clip selected";
        }
    }
    if (ImGui::GetIO().KeyMods == ImGuiMod_None &&
        ImGui::IsKeyPressed(ImGuiKey_B, false)) {
        shellState->timelineToolMode =
            shellState->timelineToolMode == TimelineToolMode::Razor
                ? TimelineToolMode::Select
                : TimelineToolMode::Razor;
        shellState->statusMessage = shellState->timelineToolMode == TimelineToolMode::Razor
            ? "razor tool enabled"
            : "select tool enabled";
    }
    if (ImGui::GetIO().KeyMods == ImGuiMod_None &&
        ImGui::IsKeyPressed(ImGuiKey_Escape, false) &&
        shellState->timelineToolMode != TimelineToolMode::Select) {
        shellState->timelineToolMode = TimelineToolMode::Select;
        shellState->statusMessage = "select tool enabled";
    }
    if (ImGui::IsKeyChordPressed(ImGuiKey_Delete)) {
        deleteSelectedClips(shellState);
    }
    if (repeatingShortcut(ImGuiKey_LeftArrow, ImGuiMod_Alt)) {
        applyCommand(shellState, jcut::NudgeSelectedClipCommand{-1});
    }
    if (repeatingShortcut(ImGuiKey_RightArrow, ImGuiMod_Alt)) {
        applyCommand(shellState, jcut::NudgeSelectedClipCommand{1});
    }
    if (ImGui::IsKeyChordPressed(ImGuiKey_Space)) {
        applyCommand(shellState, jcut::TogglePlaybackCommand{});
    }
    if (repeatingShortcut(ImGuiKey_LeftArrow, ImGuiMod_None)) {
        applyCommand(shellState, jcut::StepFrameCommand{-1});
    }
    if (repeatingShortcut(ImGuiKey_RightArrow, ImGuiMod_None)) {
        applyCommand(shellState, jcut::StepFrameCommand{1});
    }
}

void drawMenuBar(ShellState* shellState, const jcut::EditorDocumentCore& snapshot)
{
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }
    if (ImGui::BeginMenu("File")) {
        const bool projectDocumentDirty = documentIsDirty(*shellState, snapshot);
        const bool canChangeProjectIdentity =
            shellState->usesQtProjectStorage && !projectDocumentDirty;
        if (ImGui::MenuItem("Open Project")) {
            shellState->focusInspectorProjectsRequested = true;
            shellState->statusMessage = "projects panel focused";
        }
        if (ImGui::MenuItem("New Project", "Ctrl+N", false,
                            canChangeProjectIdentity)) {
            requestProjectLifecycleAction(
                shellState, ProjectLifecycleAction::NewProject, snapshot);
        }
        if (ImGui::MenuItem("Import Media")) {
            shellState->focusMediaFilesRequested = true;
            shellState->statusMessage = "media browser focused";
        }
        if (ImGui::MenuItem("Save", "Ctrl+S", false,
                            !shellState->documentPath.empty() || shellState->usesQtProjectStorage)) {
            saveCurrentDocument(shellState);
        }
        if (ImGui::MenuItem("Save Project As", "Ctrl+Shift+S", false,
                            shellState->usesQtProjectStorage)) {
            requestProjectLifecycleAction(
                shellState, ProjectLifecycleAction::SaveAs, snapshot);
        }
        if (ImGui::MenuItem("Rename Project", nullptr, false,
                            canChangeProjectIdentity)) {
            requestProjectLifecycleAction(
                shellState, ProjectLifecycleAction::Rename, snapshot);
        }
        if (ImGui::MenuItem("Reload", "Ctrl+R", false,
                            !shellState->documentPath.empty() || shellState->usesQtProjectStorage)) {
            reloadCurrentDocument(shellState);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Export")) {
            shellState->focusInspectorOutputRequested = true;
            shellState->statusMessage = "output panel focused";
        }
        ImGui::EndMenu();
    }
    bool canUndo = false;
    bool canRedo = false;
    {
        std::lock_guard<std::mutex> lock(shellState->runtimeMutex);
        canUndo = shellState->runtime.canUndo();
        canRedo = shellState->runtime.canRedo();
    }
    const jcut::EditorClip* currentClip = selectedClip(snapshot);
    const std::size_t selectionCount = selectedClipCount(snapshot);
    const bool canSplit = currentClip &&
        snapshot.transport.currentFrame > currentClip->startFrame &&
        snapshot.transport.currentFrame < currentClip->startFrame + currentClip->durationFrames;
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, canUndo)) {
            applyCommand(shellState, jcut::UndoCommand{});
        }
        if (ImGui::MenuItem("Redo", "Ctrl+Shift+Z", false, canRedo)) {
            applyCommand(shellState, jcut::RedoCommand{});
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Cut", "Ctrl+X", false, selectionCount > 0)) {
            applyCommand(shellState, jcut::CutSelectedClipsCommand{});
        }
        if (ImGui::MenuItem("Copy", "Ctrl+C", false, selectionCount > 0)) {
            applyCommand(shellState, jcut::CopySelectedClipsCommand{});
        }
        if (ImGui::MenuItem("Paste At Playhead", "Ctrl+V")) {
            applyCommand(shellState, jcut::PasteClipsCommand{
                snapshot.transport.currentFrame, selectedTrackId(snapshot)});
        }
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, selectionCount > 0)) {
            applyCommand(shellState, jcut::DuplicateSelectedClipsCommand{});
        }
        if (ImGui::MenuItem("Select All Clips", "Ctrl+A", false,
                            !snapshot.clips.empty())) {
            applyCommand(shellState, jcut::SelectAllClipsCommand{});
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Split At Playhead", "Ctrl+B", false, canSplit)) {
            applyCommand(shellState, jcut::SplitClipCommand{
                currentClip->id,
                snapshot.transport.currentFrame});
        }
        if (ImGui::MenuItem(selectionCount == 1
                                ? "Delete Selected Clip"
                                : "Delete Selected Clips",
                            "Delete", false, selectionCount > 0)) {
            deleteSelectedClips(shellState);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Nudge Left", "Alt+Left", false,
                            currentClip && currentClip->startFrame > 0)) {
            applyCommand(shellState, jcut::NudgeSelectedClipCommand{-1});
        }
        if (ImGui::MenuItem("Nudge Right", "Alt+Right", false, currentClip != nullptr)) {
            applyCommand(shellState, jcut::NudgeSelectedClipCommand{1});
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Transport")) {
        bool playing = snapshot.transport.playbackActive;
        if (ImGui::MenuItem(playing ? "Pause" : "Play", "Space", playing, true)) {
            applyCommand(shellState, jcut::TogglePlaybackCommand{});
        }
        if (ImGui::MenuItem("Previous Frame", "Left")) {
            applyCommand(shellState, jcut::StepFrameCommand{-1});
        }
        if (ImGui::MenuItem("Next Frame", "Right")) {
            applyCommand(shellState, jcut::StepFrameCommand{1});
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
        ImGui::Separator();
        if (ImGui::MenuItem("Reset Panel Layout")) {
            shellState->resetLayoutRequested = true;
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
    shellState->mediaHoveredPath.clear();
    const bool focusFiles = std::exchange(shellState->focusMediaFilesRequested, false);
    const ShellLayout layout = computeShellLayout();
    const ImGuiCond layoutCondition = shellState->resetLayoutRequested
        ? ImGuiCond_Always
        : ImGuiCond_FirstUseEver;
    ImGui::SetNextWindowPos(layout.media.pos, layoutCondition);
    ImGui::SetNextWindowSize(layout.media.size, layoutCondition);
    if (focusFiles) {
        ImGui::SetNextWindowFocus();
    }
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse;
    ImGui::Begin("Media", nullptr, flags);
    if (ImGui::BeginTabBar("MediaTabs")) {
        if (ImGui::BeginTabItem("Project")) {
            ImGui::InputText("Path", shellState->importMediaPath.data(), shellState->importMediaPath.size());
            ImGui::InputText("Label", shellState->importMediaLabel.data(), shellState->importMediaLabel.size());
            ImGui::InputText("Kind", shellState->importMediaKind.data(), shellState->importMediaKind.size());
            if (ImGui::Button("Import")) {
                applyCommand(shellState, importMediaCommandForPath(
                    shellState->importMediaPath.data(),
                    shellState->importMediaLabel.data(),
                    shellState->importMediaKind.data()));
            }
            ImGui::Separator();
            const int trackId = selectedTrackId(snapshot);
            for (const jcut::EditorMediaItem& item : snapshot.mediaItems) {
                ImGui::PushID(item.id.c_str());
                const bool selected = shellState->mediaSelectedPath == item.id;
                if (ImGui::Selectable(item.label.c_str(), selected)) {
                    shellState->mediaSelectedPath = item.id;
                }
                const bool itemHovered = ImGui::IsItemHovered();
                if (itemHovered) {
                    shellState->mediaHoveredPath = item.id;
                }
                if (itemHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    if (trackId > 0) {
                        std::int64_t probedDurationFrames = 0;
                        importMediaCommandForPath(
                            item.id,
                            item.label,
                            item.kind,
                            &probedDurationFrames);
                        applyCommand(shellState, jcut::InsertClipFromMediaCommand{
                            item.id,
                            trackId,
                            snapshot.transport.currentFrame,
                            resolvedMediaDurationFrames(
                                0, probedDurationFrames)});
                    } else {
                        shellState->statusMessage = "select a track before inserting media";
                    }
                }
                if (ImGui::BeginDragDropSource()) {
                    ImGui::SetDragDropPayload(
                        kProjectMediaDragPayload,
                        item.id.c_str(),
                        item.id.size() + 1);
                    ImGui::TextUnformatted(item.label.c_str());
                    ImGui::TextDisabled("Drop on a timeline track");
                    ImGui::EndDragDropSource();
                }
                if (ImGui::BeginPopupContextItem("ProjectMediaContext")) {
                    if (ImGui::MenuItem("Remove from Project")) {
                        const jcut::CommandResult result = applyCommand(
                            shellState, jcut::RemoveMediaCommand{item.id});
                        if (result.applied &&
                            shellState->mediaSelectedPath == item.id) {
                            shellState->mediaSelectedPath.clear();
                        }
                    }
                    ImGui::EndPopup();
                }
                ImGui::SameLine();
                ImGui::TextDisabled("(%s)", item.kind.c_str());
                ImGui::PopID();
            }
            if (!shellState->mediaSelectedPath.empty()) {
                ImGui::Separator();
                ImGui::TextWrapped("%s", shellState->mediaSelectedPath.c_str());
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Files",
                                nullptr,
                                focusFiles ? ImGuiTabItemFlags_SetSelected
                                           : ImGuiTabItemFlags_None)) {
            if (shellState->mediaRootPath[0] == '\0') {
                std::snprintf(shellState->mediaRootPath.data(),
                              shellState->mediaRootPath.size(),
                              "%s",
                              shellState->mediaRootDirectory.empty()
                                  ? (shellState->projectRootPath.empty()
                                      ? fs::current_path().string().c_str()
                                      : shellState->projectRootPath.c_str())
                                  : shellState->mediaRootDirectory.c_str());
            }
            const bool rootSubmitted = ImGui::InputText(
                "Root",
                shellState->mediaRootPath.data(),
                shellState->mediaRootPath.size(),
                ImGuiInputTextFlags_EnterReturnsTrue);
            if (rootSubmitted || ImGui::IsItemDeactivatedAfterEdit()) {
                if (!applyMediaRootPath(shellState, shellState->mediaRootPath.data())) {
                    std::snprintf(shellState->mediaRootPath.data(),
                                  shellState->mediaRootPath.size(),
                                  "%s",
                                  shellState->mediaRootDirectory.c_str());
                }
            }
            ImGui::InputText("Filter", shellState->mediaBrowserFilter.data(), shellState->mediaBrowserFilter.size());
            if (ImGui::Button("Use Project Root")) {
                applyMediaRootPath(shellState, shellState->projectRootPath);
            }
            ImGui::SameLine();
            if (ImGui::Button("Up")) {
                fs::path current = shellState->mediaGalleryPath.empty()
                    ? fs::path(shellState->mediaRootPath.data())
                    : fs::path(shellState->mediaGalleryPath);
                if (current.has_parent_path()) {
                    const std::string parent = pathString(current.parent_path());
                    applyMediaRootPath(shellState, parent);
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
                    const bool isSequence = isDir &&
                        jcut::isImageSequenceDirectory(entryPath);
                    const bool importable = isSequence ||
                        (!isDir && isMediaFilePath(entryPath));
                    const bool selectable = isDir || importable;
                    const std::string entryPathText = pathString(entryPath);
                    ImGui::PushID(entryPathText.c_str());
                    std::string label = isSequence
                        ? "[sequence] " + entryPath.filename().string()
                        : isDir
                        ? "[dir] " + entryPath.filename().string()
                        : "[" + mediaKindForPath(entryPath) + "] " + entryPath.filename().string();
                    if (!selectable) {
                        ImGui::BeginDisabled();
                    }
                    const bool selected = shellState->mediaSelectedPath == pathString(entryPath);
                    if (ImGui::Selectable(label.c_str(), selected)) {
                        shellState->mediaSelectedPath = entryPathText;
                    }
                    if (ImGui::IsItemHovered()) {
                        shellState->mediaHoveredPath = entryPathText;
                    }
                    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        if (isDir && !isSequence) {
                            shellState->mediaGalleryPath = pathString(entryPath);
                        } else if (importable) {
                            importFilesystemMedia(shellState, snapshot, entryPath, true);
                        }
                    }
                    if (importable && ImGui::BeginDragDropSource()) {
                        ImGui::SetDragDropPayload(
                            kFilesystemMediaDragPayload,
                            entryPathText.c_str(),
                            entryPathText.size() + 1);
                        ImGui::TextUnformatted(entryPath.filename().string().c_str());
                        ImGui::TextDisabled("Drop on a timeline track");
                        ImGui::EndDragDropSource();
                    }
                    if (!selectable) {
                        ImGui::EndDisabled();
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();
            const std::string previewPath = !shellState->mediaHoveredPath.empty()
                ? shellState->mediaHoveredPath
                : shellState->mediaSelectedPath;
            if (!previewPath.empty()) {
                ImGui::Separator();
                ImGui::TextWrapped("%s", previewPath.c_str());
                const bool previewImportable = isImportableMediaPath(previewPath);
                if (previewImportable && ImGui::Button("Import Selected")) {
                    importFilesystemMedia(shellState, snapshot, previewPath, false);
                }
                ImGui::SameLine();
                if (previewImportable && ImGui::Button("Insert Selected")) {
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
    const ImGuiCond layoutCondition = shellState->resetLayoutRequested
        ? ImGuiCond_Always
        : ImGuiCond_FirstUseEver;
    ImGui::SetNextWindowPos(layout.preview.pos, layoutCondition);
    ImGui::SetNextWindowSize(layout.preview.size, layoutCondition);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse;
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
    const jcut::EditorClip* previewTitleClip = selectedClip(snapshot);
    const bool selectedTitleIsActive = previewTitleClip &&
        previewTitleClip->mediaKind == "title" &&
        snapshot.transport.currentFrame >= previewTitleClip->startFrame &&
        snapshot.transport.currentFrame <
            previewTitleClip->startFrame + std::max(1, previewTitleClip->durationFrames);
    const bool selectedTransformClipIsActive = previewTitleClip &&
        previewTitleClip->mediaKind != "audio" &&
        previewTitleClip->mediaKind != "title" &&
        snapshot.transport.currentFrame >= previewTitleClip->startFrame &&
        snapshot.transport.currentFrame <
            previewTitleClip->startFrame + std::max(1, previewTitleClip->durationFrames);
    const jcut::EditorClip* correctionClip = previewTitleClip;
    const bool correctionClipIsActive = correctionClip &&
        correctionClip->mediaKind != "audio" &&
        shellState->correctionClipId == correctionClip->id &&
        snapshot.transport.currentFrame >= correctionClip->startFrame &&
        snapshot.transport.currentFrame <
            correctionClip->startFrame + std::max(1, correctionClip->durationFrames);
    const bool correctionInteractionActive = correctionClipIsActive &&
        (shellState->correctionDrawMode || shellState->selectedCorrectionPolygon >= 0 ||
         shellState->correctionPointDragActive);
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
        ((!selectedTitleIsActive && !selectedTransformClipIsActive &&
          !correctionInteractionActive &&
          ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) ||
         ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f) ||
         ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f))) {
        shellState->previewPanX += io.MouseDelta.x;
        shellState->previewPanY += io.MouseDelta.y;
    }
    if (mouseInsideCanvas && !selectedTitleIsActive &&
        !selectedTransformClipIsActive && !correctionInteractionActive &&
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
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
    const bool mouseInsideProgram = ImGui::IsMouseHoveringRect(frameMin, frameMax);

    const auto correctionPointToScreen = [&](const jcut::EditorPoint& point) {
        return ImVec2(
            frameMin.x + static_cast<float>(point.x) * frameWidth,
            frameMin.y + static_cast<float>(point.y) * frameHeight);
    };
    const auto correctionPointFromScreen = [&](const ImVec2& point) {
        return jcut::EditorPoint{
            std::clamp(static_cast<double>((point.x - frameMin.x) /
                                           std::max(1.0f, frameWidth)), 0.0, 1.0),
            std::clamp(static_cast<double>((point.y - frameMin.y) /
                                           std::max(1.0f, frameHeight)), 0.0, 1.0)};
    };

    if (!correctionClipIsActive && shellState->correctionPointDragActive) {
        shellState->correctionPointDragActive = false;
        shellState->correctionPointDragPolygon = -1;
        shellState->correctionPointDragPoint = -1;
        endRuntimeHistoryTransaction(shellState);
    }
    if (correctionClipIsActive && mouseInsideProgram &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (shellState->correctionDrawMode) {
            shellState->correctionDraftPoints.push_back(
                correctionPointFromScreen(io.MousePos));
        } else {
            constexpr float kPointHitRadius = 9.0f;
            float bestDistanceSquared = kPointHitRadius * kPointHitRadius;
            int hitPolygon = -1;
            int hitPoint = -1;
            for (std::size_t polygonIndex = 0;
                 polygonIndex < correctionClip->correctionPolygons.size();
                 ++polygonIndex) {
                const auto& points = correctionClip->correctionPolygons[polygonIndex].pointsNormalized;
                for (std::size_t pointIndex = 0; pointIndex < points.size(); ++pointIndex) {
                    const ImVec2 screenPoint = correctionPointToScreen(points[pointIndex]);
                    const float deltaX = io.MousePos.x - screenPoint.x;
                    const float deltaY = io.MousePos.y - screenPoint.y;
                    const float distanceSquared = deltaX * deltaX + deltaY * deltaY;
                    if (distanceSquared <= bestDistanceSquared) {
                        bestDistanceSquared = distanceSquared;
                        hitPolygon = static_cast<int>(polygonIndex);
                        hitPoint = static_cast<int>(pointIndex);
                    }
                }
            }
            if (hitPolygon >= 0) {
                shellState->selectedCorrectionPolygon = hitPolygon;
                shellState->correctionPointDragActive = true;
                shellState->correctionPointDragPolygon = hitPolygon;
                shellState->correctionPointDragPoint = hitPoint;
                shellState->correctionPointDragPolygons = correctionClip->correctionPolygons;
                beginRuntimeHistoryTransaction(shellState);
            }
        }
    }
    if (shellState->correctionPointDragActive && correctionClipIsActive &&
        ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const int polygonIndex = shellState->correctionPointDragPolygon;
        const int pointIndex = shellState->correctionPointDragPoint;
        if (polygonIndex >= 0 &&
            polygonIndex < static_cast<int>(shellState->correctionPointDragPolygons.size()) &&
            pointIndex >= 0 && pointIndex < static_cast<int>(
                shellState->correctionPointDragPolygons[polygonIndex].pointsNormalized.size())) {
            shellState->correctionPointDragPolygons[polygonIndex].pointsNormalized[pointIndex] =
                correctionPointFromScreen(io.MousePos);
            applyCommand(shellState, jcut::SetClipCorrectionPolygonsCommand{
                correctionClip->id, shellState->correctionPointDragPolygons});
        }
    }
    if (shellState->correctionPointDragActive &&
        ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        shellState->correctionPointDragActive = false;
        shellState->correctionPointDragPolygon = -1;
        shellState->correctionPointDragPoint = -1;
        endRuntimeHistoryTransaction(shellState);
    }

    jcut::EditorTransformKeyframe selectedPreviewTransform;
    ImVec2 transformBoundsMin{};
    ImVec2 transformBoundsMax{};
    ImVec2 transformRightHandleMin{};
    ImVec2 transformRightHandleMax{};
    ImVec2 transformBottomHandleMin{};
    ImVec2 transformBottomHandleMax{};
    ImVec2 transformCornerHandleMin{};
    ImVec2 transformCornerHandleMax{};
    if (selectedTransformClipIsActive) {
        selectedPreviewTransform = shellState->previewTransformDragMode !=
                PreviewTransformDragMode::None
            ? shellState->previewTransformDragValue
            : jcut::evaluateEditorClipTransformAtLocalFrame(
                  *previewTitleClip,
                  snapshot.transport.currentFrame - previewTitleClip->startFrame);
        const double radians = selectedPreviewTransform.rotation *
            3.14159265358979323846 / 180.0;
        const float scaledWidth = frameWidth * static_cast<float>(
            std::abs(selectedPreviewTransform.scaleX));
        const float scaledHeight = frameHeight * static_cast<float>(
            std::abs(selectedPreviewTransform.scaleY));
        const float boundsWidth = std::abs(static_cast<float>(std::cos(radians))) * scaledWidth +
            std::abs(static_cast<float>(std::sin(radians))) * scaledHeight;
        const float boundsHeight = std::abs(static_cast<float>(std::sin(radians))) * scaledWidth +
            std::abs(static_cast<float>(std::cos(radians))) * scaledHeight;
        const float outputWidth = static_cast<float>(
            std::max(1, snapshot.exportRequest.outputSize.width));
        const float outputHeight = static_cast<float>(
            std::max(1, snapshot.exportRequest.outputSize.height));
        const ImVec2 center(
            (frameMin.x + frameMax.x) * 0.5f +
                static_cast<float>(selectedPreviewTransform.translationX) *
                    frameWidth / outputWidth,
            (frameMin.y + frameMax.y) * 0.5f +
                static_cast<float>(selectedPreviewTransform.translationY) *
                    frameHeight / outputHeight);
        transformBoundsMin = {center.x - boundsWidth * 0.5f,
                              center.y - boundsHeight * 0.5f};
        transformBoundsMax = {center.x + boundsWidth * 0.5f,
                              center.y + boundsHeight * 0.5f};
        constexpr float kHandleRadius = 7.0f;
        transformRightHandleMin = {
            transformBoundsMax.x - kHandleRadius,
            center.y - kHandleRadius};
        transformRightHandleMax = {
            transformBoundsMax.x + kHandleRadius,
            center.y + kHandleRadius};
        transformBottomHandleMin = {
            center.x - kHandleRadius,
            transformBoundsMax.y - kHandleRadius};
        transformBottomHandleMax = {
            center.x + kHandleRadius,
            transformBoundsMax.y + kHandleRadius};
        transformCornerHandleMin = {
            transformBoundsMax.x - kHandleRadius,
            transformBoundsMax.y - kHandleRadius};
        transformCornerHandleMax = {
            transformBoundsMax.x + kHandleRadius,
            transformBoundsMax.y + kHandleRadius};
    }
    const auto pointInRect = [](const ImVec2& point, const ImVec2& minimum,
                                const ImVec2& maximum) {
        return point.x >= minimum.x && point.x <= maximum.x &&
            point.y >= minimum.y && point.y <= maximum.y;
    };
    if ((!selectedTransformClipIsActive ||
         shellState->previewTransformDragClipId != previewTitleClip->id) &&
        shellState->previewTransformDragMode != PreviewTransformDragMode::None) {
        shellState->previewTransformDragMode = PreviewTransformDragMode::None;
        shellState->previewTransformDragClipId = -1;
        endRuntimeHistoryTransaction(shellState);
    }
    if (selectedTransformClipIsActive && !correctionInteractionActive &&
        mouseInsideProgram && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        PreviewTransformDragMode dragMode = PreviewTransformDragMode::None;
        if (pointInRect(io.MousePos, transformCornerHandleMin, transformCornerHandleMax)) {
            dragMode = PreviewTransformDragMode::ResizeBoth;
        } else if (pointInRect(io.MousePos, transformRightHandleMin, transformRightHandleMax)) {
            dragMode = PreviewTransformDragMode::ResizeX;
        } else if (pointInRect(io.MousePos, transformBottomHandleMin, transformBottomHandleMax)) {
            dragMode = PreviewTransformDragMode::ResizeY;
        } else if (pointInRect(io.MousePos, transformBoundsMin, transformBoundsMax)) {
            dragMode = PreviewTransformDragMode::Move;
        }
        if (dragMode != PreviewTransformDragMode::None) {
            shellState->previewTransformDragMode = dragMode;
            shellState->previewTransformDragClipId = previewTitleClip->id;
            shellState->previewTransformDragOriginMouse = io.MousePos;
            shellState->previewTransformDragOriginBoundsMin = transformBoundsMin;
            shellState->previewTransformDragOriginBoundsMax = transformBoundsMax;
            shellState->previewTransformDragValue = selectedPreviewTransform;
            beginRuntimeHistoryTransaction(shellState);
        }
    }
    if (shellState->previewTransformDragMode != PreviewTransformDragMode::None &&
        selectedTransformClipIsActive && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const float deltaX = io.MousePos.x - shellState->previewTransformDragOriginMouse.x;
        const float deltaY = io.MousePos.y - shellState->previewTransformDragOriginMouse.y;
        if (std::abs(deltaX) > 0.0001f || std::abs(deltaY) > 0.0001f) {
            jcut::EditorTransformKeyframe next = shellState->previewTransformDragValue;
            const float outputWidth = static_cast<float>(
                std::max(1, snapshot.exportRequest.outputSize.width));
            const float outputHeight = static_cast<float>(
                std::max(1, snapshot.exportRequest.outputSize.height));
            const double previewScaleX = frameWidth / outputWidth;
            const double previewScaleY = frameHeight / outputHeight;
            if (shellState->previewTransformDragMode == PreviewTransformDragMode::Move) {
                next.translationX += deltaX / std::max(0.0001, previewScaleX);
                next.translationY += deltaY / std::max(0.0001, previewScaleY);
            } else {
                const double originWidth = std::max(
                    1.0f, shellState->previewTransformDragOriginBoundsMax.x -
                              shellState->previewTransformDragOriginBoundsMin.x);
                const double originHeight = std::max(
                    1.0f, shellState->previewTransformDragOriginBoundsMax.y -
                              shellState->previewTransformDragOriginBoundsMin.y);
                double factorX = 1.0 + deltaX / originWidth;
                double factorY = 1.0 + deltaY / originHeight;
                jcut::preview::ResizeAnchor anchor = jcut::preview::ResizeAnchor::Center;
                if (shellState->previewTransformDragMode == PreviewTransformDragMode::ResizeX) {
                    factorY = 1.0;
                    anchor = jcut::preview::ResizeAnchor::Left;
                } else if (shellState->previewTransformDragMode == PreviewTransformDragMode::ResizeY) {
                    factorX = 1.0;
                    anchor = jcut::preview::ResizeAnchor::Top;
                } else {
                    const double uniformFactor =
                        std::abs(factorX) >= std::abs(factorY) ? factorX : factorY;
                    factorX = uniformFactor;
                    factorY = uniformFactor;
                    anchor = jcut::preview::ResizeAnchor::TopLeft;
                }
                const auto boundedScale = [](double value) {
                    if (std::abs(value) >= 0.01) return value;
                    return value < 0.0 ? -0.01 : 0.01;
                };
                next.scaleX = boundedScale(next.scaleX * factorX);
                next.scaleY = boundedScale(next.scaleY * factorY);
                const jcut::preview::PointD translation =
                    jcut::preview::translationForAnchoredResize(
                        {shellState->previewTransformDragValue.translationX,
                         shellState->previewTransformDragValue.translationY},
                        {shellState->previewTransformDragValue.scaleX,
                         shellState->previewTransformDragValue.scaleY},
                        {next.scaleX, next.scaleY},
                        {shellState->previewTransformDragOriginBoundsMin.x,
                         shellState->previewTransformDragOriginBoundsMin.y,
                         originWidth, originHeight},
                        anchor,
                        {previewScaleX, previewScaleY});
                next.translationX = translation.x;
                next.translationY = translation.y;
            }
            next.frame = snapshot.transport.currentFrame - previewTitleClip->startFrame;
            shellState->previewTransformDragValue = next;
            shellState->previewTransformDragOriginMouse = io.MousePos;
            shellState->previewTransformDragOriginBoundsMin = transformBoundsMin;
            shellState->previewTransformDragOriginBoundsMax = transformBoundsMax;
            applyCommand(shellState, jcut::CommitPreviewTransformCommand{
                previewTitleClip->id, next.frame, next.translationX, next.translationY,
                next.rotation, next.scaleX, next.scaleY});
        }
    }
    if (shellState->previewTransformDragMode != PreviewTransformDragMode::None &&
        ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        shellState->previewTransformDragMode = PreviewTransformDragMode::None;
        shellState->previewTransformDragClipId = -1;
        endRuntimeHistoryTransaction(shellState);
    }

    if ((!selectedTitleIsActive ||
         shellState->previewTitleDragClipId != previewTitleClip->id) &&
        shellState->previewTitleDragActive) {
        shellState->previewTitleDragActive = false;
        shellState->previewTitleDragClipId = -1;
        endRuntimeHistoryTransaction(shellState);
    }
    if (selectedTitleIsActive && !correctionInteractionActive &&
        !shellState->correctionPointDragActive && mouseInsideProgram &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        shellState->previewTitleDragActive = true;
        shellState->previewTitleDragClipId = previewTitleClip->id;
        shellState->previewTitleDragKeyframe =
            jcut::evaluateEditorClipTitleAtLocalFrame(
                *previewTitleClip,
                snapshot.transport.currentFrame - previewTitleClip->startFrame);
        shellState->previewTitleDragKeyframe.frame =
            snapshot.transport.currentFrame - previewTitleClip->startFrame;
        beginRuntimeHistoryTransaction(shellState);
    }
    if (shellState->previewTitleDragActive && selectedTitleIsActive &&
        ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const double outputWidth = std::max(1, snapshot.exportRequest.outputSize.width);
        const double outputHeight = std::max(1, snapshot.exportRequest.outputSize.height);
        const double deltaX = io.MouseDelta.x * outputWidth /
            std::max(1.0f, frameWidth);
        const double deltaY = io.MouseDelta.y * outputHeight /
            std::max(1.0f, frameHeight);
        if (std::abs(deltaX) > 0.0001 || std::abs(deltaY) > 0.0001) {
            shellState->previewTitleDragKeyframe.translationX += deltaX;
            shellState->previewTitleDragKeyframe.translationY += deltaY;
            shellState->titleDraftClipId = previewTitleClip->id;
            shellState->titleDraft = shellState->previewTitleDragKeyframe;
            applyCommand(shellState, jcut::UpsertTitleKeyframeCommand{
                previewTitleClip->id,
                shellState->previewTitleDragKeyframe});
        }
    }
    if (shellState->previewTitleDragActive &&
        ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        shellState->previewTitleDragActive = false;
        shellState->previewTitleDragClipId = -1;
        endRuntimeHistoryTransaction(shellState);
    }

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
    const std::string previewFaceClipIdentity = previewTitleClip
        ? (previewTitleClip->persistentId.empty()
            ? std::to_string(previewTitleClip->id)
            : previewTitleClip->persistentId)
        : std::string{};
    const std::string previewFaceContextSuffix =
        "::" + previewFaceClipIdentity;
    if (previewTitleClip &&
        shellState->transcriptCache.faceArtifactContext.ends_with(
            previewFaceContextSuffix) &&
        shellState->transcriptCache.selectedFaceTrackIds.size() == 1) {
        const int selectedTrackId =
            shellState->transcriptCache.selectedFaceTrackIds.front();
        const auto selectedTrack = std::find_if(
            shellState->transcriptCache.faceInspection.tracks.begin(),
            shellState->transcriptCache.faceInspection.tracks.end(),
            [&](const auto& track) {
                return track.trackId == selectedTrackId;
            });
        if (selectedTrack !=
            shellState->transcriptCache.faceInspection.tracks.end()) {
            const float centerX = frameMin.x +
                static_cast<float>(selectedTrack->x) * frameWidth;
            const float centerY = frameMin.y +
                static_cast<float>(selectedTrack->y) * frameHeight;
            const float boxSize = static_cast<float>(
                std::clamp(selectedTrack->box, 0.01, 1.0));
            const float halfWidth = boxSize * frameWidth * 0.5f;
            const float halfHeight = boxSize * frameHeight * 0.5f;
            const ImVec2 faceMin(
                std::max(frameMin.x, centerX - halfWidth),
                std::max(frameMin.y, centerY - halfHeight));
            const ImVec2 faceMax(
                std::min(frameMax.x, centerX + halfWidth),
                std::min(frameMax.y, centerY + halfHeight));
            drawList->AddRect(
                faceMin, faceMax, IM_COL32(92, 230, 150, 255),
                3.0f, 0, 3.0f);
            const std::string referenceLabel =
                "Face track " + std::to_string(selectedTrackId);
            drawList->AddText(
                ImVec2(faceMin.x, std::max(frameMin.y, faceMin.y - 20.0f)),
                IM_COL32(92, 230, 150, 255),
                referenceLabel.c_str());
        }
    }
    if (selectedTransformClipIsActive && !correctionInteractionActive) {
        const ImU32 outlineColor = shellState->previewTransformDragMode !=
                PreviewTransformDragMode::None
            ? IM_COL32(255, 196, 92, 255)
            : IM_COL32(92, 196, 255, 235);
        drawList->AddRect(transformBoundsMin, transformBoundsMax,
                          outlineColor, 2.0f, 0, 2.0f);
        const auto drawHandle = [&](const ImVec2& minimum, const ImVec2& maximum) {
            drawList->AddRectFilled(minimum, maximum, IM_COL32(24, 28, 34, 245), 2.0f);
            drawList->AddRect(minimum, maximum, outlineColor, 2.0f, 0, 2.0f);
        };
        drawHandle(transformRightHandleMin, transformRightHandleMax);
        drawHandle(transformBottomHandleMin, transformBottomHandleMax);
        drawHandle(transformCornerHandleMin, transformCornerHandleMax);
        if (mouseInsideProgram) {
            if (pointInRect(io.MousePos, transformCornerHandleMin, transformCornerHandleMax)) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
            } else if (pointInRect(io.MousePos, transformRightHandleMin, transformRightHandleMax)) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            } else if (pointInRect(io.MousePos, transformBottomHandleMin, transformBottomHandleMax)) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            } else if (pointInRect(io.MousePos, transformBoundsMin, transformBoundsMax)) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            }
        }
    }
    if (correctionClipIsActive) {
        const std::int64_t localFrame =
            snapshot.transport.currentFrame - correctionClip->startFrame;
        for (std::size_t polygonIndex = 0;
             polygonIndex < correctionClip->correctionPolygons.size();
             ++polygonIndex) {
            const auto& polygon = correctionClip->correctionPolygons[polygonIndex];
            if (polygon.pointsNormalized.empty()) {
                continue;
            }
            const bool inRange = polygon.enabled && localFrame >= polygon.startFrame &&
                (polygon.endFrame < 0 || localFrame <= polygon.endFrame);
            const bool selected = shellState->selectedCorrectionPolygon ==
                static_cast<int>(polygonIndex);
            const ImU32 lineColor = selected
                ? IM_COL32(255, 196, 72, 255)
                : (inRange ? IM_COL32(255, 92, 92, 235) : IM_COL32(130, 136, 146, 180));
            for (std::size_t pointIndex = 0;
                 pointIndex < polygon.pointsNormalized.size();
                 ++pointIndex) {
                const ImVec2 point = correctionPointToScreen(
                    polygon.pointsNormalized[pointIndex]);
                if (polygon.pointsNormalized.size() > 1) {
                    const ImVec2 next = correctionPointToScreen(
                        polygon.pointsNormalized[(pointIndex + 1) %
                                                 polygon.pointsNormalized.size()]);
                    drawList->AddLine(point, next, lineColor, selected ? 2.5f : 1.5f);
                }
                drawList->AddCircleFilled(point, selected ? 5.0f : 3.5f,
                                          IM_COL32(24, 26, 30, 245));
                drawList->AddCircle(point, selected ? 5.0f : 3.5f, lineColor, 0, 2.0f);
            }
        }
        for (std::size_t pointIndex = 0;
             pointIndex < shellState->correctionDraftPoints.size();
             ++pointIndex) {
            const ImVec2 point = correctionPointToScreen(
                shellState->correctionDraftPoints[pointIndex]);
            if (pointIndex > 0) {
                drawList->AddLine(
                    correctionPointToScreen(shellState->correctionDraftPoints[pointIndex - 1]),
                    point, IM_COL32(92, 220, 255, 255), 2.0f);
            }
            drawList->AddCircleFilled(point, 4.5f, IM_COL32(92, 220, 255, 255));
        }
        if (mouseInsideProgram) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
    }
    if (selectedTitleIsActive) {
        const jcut::EditorTitleKeyframe title =
            shellState->previewTitleDragActive
            ? shellState->previewTitleDragKeyframe
            : jcut::evaluateEditorClipTitleAtLocalFrame(
                  *previewTitleClip,
                  snapshot.transport.currentFrame - previewTitleClip->startFrame);
        std::size_t maximumLineCharacters = 0;
        std::size_t lineCharacters = 0;
        int lineCount = 1;
        for (const char character : title.text) {
            if (character == '\n') {
                maximumLineCharacters = std::max(maximumLineCharacters, lineCharacters);
                lineCharacters = 0;
                ++lineCount;
            } else if ((static_cast<unsigned char>(character) & 0xc0) != 0x80) {
                ++lineCharacters;
            }
        }
        maximumLineCharacters = std::max(maximumLineCharacters, lineCharacters);
        const float outputWidth = static_cast<float>(
            std::max(1, snapshot.exportRequest.outputSize.width));
        const float outputHeight = static_cast<float>(
            std::max(1, snapshot.exportRequest.outputSize.height));
        const float titleCenterX = frameMin.x + frameWidth * 0.5f +
            static_cast<float>(title.translationX) * frameWidth / outputWidth;
        const float titleCenterY = frameMin.y + frameHeight * 0.5f +
            static_cast<float>(title.translationY) * frameHeight / outputHeight;
        const float renderedFontHeight = std::max(
            12.0f, static_cast<float>(title.fontSize) * frameHeight / outputHeight);
        const float boundsWidth = std::max(
            48.0f, static_cast<float>(maximumLineCharacters) * renderedFontHeight * 0.64f + 16.0f);
        const float boundsHeight = std::max(
            28.0f, renderedFontHeight * static_cast<float>(lineCount) * 1.25f + 12.0f);
        const ImVec2 titleBoundsMin(
            titleCenterX - boundsWidth * 0.5f,
            titleCenterY - boundsHeight * 0.5f);
        const ImVec2 titleBoundsMax(
            titleCenterX + boundsWidth * 0.5f,
            titleCenterY + boundsHeight * 0.5f);
        drawList->AddRect(
            titleBoundsMin, titleBoundsMax,
            shellState->previewTitleDragActive
                ? IM_COL32(255, 196, 92, 255)
                : IM_COL32(92, 196, 255, 230),
            2.0f, 0, 2.0f);
        drawList->AddCircleFilled(
            ImVec2(titleCenterX, titleCenterY), 3.5f,
            IM_COL32(255, 255, 255, 240));
        if (mouseInsideProgram && !correctionInteractionActive) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        }
    }
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
    const ImGuiCond layoutCondition = shellState->resetLayoutRequested
        ? ImGuiCond_Always
        : ImGuiCond_FirstUseEver;
    ImGui::SetNextWindowPos(layout.timeline.pos, layoutCondition);
    ImGui::SetNextWindowSize(layout.timeline.size, layoutCondition);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse;
    ImGui::Begin("Timeline", nullptr, flags);
    if (!ImGui::GetDragDropPayload() &&
        shellState->timelineDragMode == TimelineDragMode::None) {
        shellState->timelineSnapIndicatorFrame = -1;
    }
    if (ImGui::RadioButton("Select", shellState->timelineToolMode == TimelineToolMode::Select)) {
        shellState->timelineToolMode = TimelineToolMode::Select;
        shellState->statusMessage = "select tool enabled";
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Razor (B)", shellState->timelineToolMode == TimelineToolMode::Razor)) {
        shellState->timelineToolMode = TimelineToolMode::Razor;
        shellState->statusMessage = "razor tool enabled";
    }
    ImGui::SameLine();
    ImGui::Checkbox("Snap", &shellState->timelineSnappingEnabled);
    ImGui::Separator();
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
    bool hoveredClipIsMaskMatte = false;
    TimelineDragMode hoveredMode = TimelineDragMode::None;
    const jcut::EditorRenderSyncMarker* hoveredRenderSyncMarker = nullptr;
    int hoveredRenderSyncClipId = 0;
    int hoveredRenderSyncTrackId = 0;
    const ImVec2 mousePos = ImGui::GetIO().MousePos;

    for (std::size_t i = 0; i < snapshot.tracks.size(); ++i) {
        const jcut::EditorTrack& track = snapshot.tracks[i];
        const bool generatedChildTrack =
            jcut::isGeneratedEditorChildTrack(track);
        const std::string timelineTrackLabel = generatedChildTrack
            ? std::string(kGeneratedTrackLabelPrefix) + track.label
            : track.label;
        const float y = origin.y + kTimelineTopPadding + static_cast<float>(i) * kTimelineRowHeight;
        drawList->AddText(
            ImVec2(origin.x + 10.0f, y + 6.0f),
            track.selected
                ? IM_COL32(255, 214, 140, 255)
                : (generatedChildTrack
                       ? IM_COL32(158, 168, 178, 255)
                       : IM_COL32(224, 228, 232, 255)),
            timelineTrackLabel.c_str());
        drawList->AddRectFilled(ImVec2(origin.x + kTimelineLabelWidth, y),
                                ImVec2(origin.x + avail.x - 12.0f, y + kTimelineClipHeight),
                                generatedChildTrack
                                    ? kGeneratedTrackLaneColor
                                    : IM_COL32(34, 38, 44, 255),
                                4.0f);
        for (const jcut::EditorClip& clip : snapshot.clips) {
            if (clip.trackId != track.id) {
                continue;
            }
            const bool maskMatteClip =
                jcut::canonicalEditorClipRole(clip.clipRole) == "mask_matte";
            const float clipStart = origin.x + kTimelineLabelWidth + kTimelineTrackPadding +
                static_cast<float>(clip.startFrame) * kTimelinePixelsPerFrame;
            const float clipWidth = std::max(40.0f, static_cast<float>(clip.durationFrames) * kTimelinePixelsPerFrame);
            const ImVec2 clipMin(clipStart, y + 2.0f);
            const ImVec2 clipMax(clipStart + clipWidth, y + kTimelineClipHeight - 2.0f);
            const ImU32 color = generatedChildTrack
                ? (clip.selected
                       ? kGeneratedTrackSelectedClipColor
                       : kGeneratedTrackClipColor)
                : (clip.selected
                       ? IM_COL32(255, 196, 86, 255)
                       : trackColors[i % trackColors.size()]);
            drawList->AddRectFilled(clipMin,
                                    clipMax,
                                    color,
                                    4.0f);
            drawList->AddText(ImVec2(clipStart + 8.0f, y + 6.0f), IM_COL32(245, 245, 245, 255), clip.label.c_str());
            if (clip.locked) {
                drawList->AddText(
                    ImVec2(std::max(clipMin.x + 8.0f, clipMax.x - 42.0f),
                           y + 6.0f),
                    IM_COL32(255, 226, 160, 255),
                    "LOCK");
            }
            if (clip.selected && !clip.locked && !maskMatteClip) {
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
                hoveredClipIsMaskMatte = maskMatteClip;
                hoveredMode = (shellState->timelineToolMode == TimelineToolMode::Razor ||
                               clip.locked || maskMatteClip)
                    ? TimelineDragMode::None
                    : TimelineDragMode::MoveClip;
                if (shellState->timelineToolMode == TimelineToolMode::Select &&
                    !clip.locked) {
                    if (clip.selected && mousePos.x <= clipMin.x + kTimelineHandleWidth) {
                        hoveredMode = TimelineDragMode::TrimClipStart;
                    } else if (clip.selected && mousePos.x >= clipMax.x - kTimelineHandleWidth) {
                        hoveredMode = TimelineDragMode::TrimClipEnd;
                    }
                }
            }
        }
    }

    // Render-sync decisions belong to a persistent source clip and occupy a
    // timeline frame, so draw and hit-test them independently of selection.
    // This mirrors the Qt timeline's visible marker affordance while keeping
    // marker mutation in the shared runtime commands.
    for (const jcut::EditorRenderSyncMarker& marker :
         snapshot.renderSyncMarkers) {
        const auto clipIt = std::find_if(
            snapshot.clips.begin(), snapshot.clips.end(),
            [&](const jcut::EditorClip& clip) {
                return clip.persistentId == marker.clipId &&
                    jcut::canonicalEditorClipRole(clip.clipRole) !=
                        "mask_matte";
            });
        if (clipIt == snapshot.clips.end() ||
            marker.frame < clipIt->startFrame ||
            marker.frame >= clipIt->startFrame + clipIt->durationFrames) {
            continue;
        }
        const auto trackIt = std::find_if(
            snapshot.tracks.begin(), snapshot.tracks.end(),
            [&](const jcut::EditorTrack& track) {
                return track.id == clipIt->trackId;
            });
        if (trackIt == snapshot.tracks.end()) {
            continue;
        }
        const std::size_t trackIndex = static_cast<std::size_t>(
            std::distance(snapshot.tracks.begin(), trackIt));
        const float markerX = origin.x + kTimelineLabelWidth +
            kTimelineTrackPadding +
            static_cast<float>(marker.frame) * kTimelinePixelsPerFrame;
        const float markerY = origin.y + kTimelineTopPadding +
            static_cast<float>(trackIndex) * kTimelineRowHeight + 2.0f;
        const ImVec2 markerMin(markerX - 3.0f, markerY);
        const ImVec2 markerMax(
            markerX + 3.0f, markerY + kTimelineClipHeight - 4.0f);
        const ImU32 markerColor = marker.skipFrame
            ? IM_COL32(255, 158, 61, 235)
            : IM_COL32(255, 91, 91, 235);
        drawList->AddRectFilled(markerMin, markerMax, markerColor, 3.0f);
        drawList->AddRect(
            markerMin, markerMax, IM_COL32(92, 45, 35, 255), 3.0f);
        if (ImGui::IsMouseHoveringRect(markerMin, markerMax)) {
            hoveredRenderSyncMarker = &marker;
            hoveredRenderSyncClipId = clipIt->id;
            hoveredRenderSyncTrackId = clipIt->trackId;
        }
    }
    if (hoveredRenderSyncMarker) {
        ImGui::SetTooltip(
            "%s %d frame%s at %lld",
            hoveredRenderSyncMarker->skipFrame ? "Skip" : "Duplicate",
            hoveredRenderSyncMarker->count,
            hoveredRenderSyncMarker->count == 1 ? "" : "s",
            static_cast<long long>(hoveredRenderSyncMarker->frame));
    }

    if (shellState->timelineToolMode == TimelineToolMode::Razor &&
        hoveredClipId != 0 && !hoveredClipIsMaskMatte) {
        const int razorFrame = frameFromTimelineX(origin.x, mousePos.x);
        const float razorX = origin.x + kTimelineLabelWidth + kTimelineTrackPadding +
            static_cast<float>(razorFrame) * kTimelinePixelsPerFrame;
        drawList->AddLine(ImVec2(razorX, origin.y + 4.0f),
                          ImVec2(razorX, origin.y + avail.y - 4.0f),
                          IM_COL32(120, 220, 255, 230),
                          2.0f);
    }

    if (shellState->timelineSnapIndicatorFrame >= 0) {
        const float snapX = origin.x + kTimelineLabelWidth + kTimelineTrackPadding +
            static_cast<float>(shellState->timelineSnapIndicatorFrame) * kTimelinePixelsPerFrame;
        drawList->AddLine(ImVec2(snapX, origin.y + 4.0f),
                          ImVec2(snapX, origin.y + avail.y - 4.0f),
                          IM_COL32(92, 232, 178, 230),
                          2.0f);
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
            if (shellState->timelineToolMode == TimelineToolMode::Razor &&
                !hoveredClipIsMaskMatte) {
                const auto hoveredIt = std::find_if(
                    snapshot.clips.begin(), snapshot.clips.end(),
                    [&](const jcut::EditorClip& clip) {
                        return clip.id == hoveredClipId;
                    });
                applyCommand(shellState, jcut::SelectTrackCommand{hoveredTrackId});
                if (hoveredIt != snapshot.clips.end() && !hoveredIt->selected) {
                    applyCommand(shellState, jcut::SelectClipCommand{hoveredClipId});
                }
                applyCommand(shellState, jcut::SplitSelectedClipsCommand{
                    frameFromTimelineX(origin.x, mousePos.x)});
                clearTimelineDrag(shellState);
            } else {
                const ImGuiKeyChord keyMods = ImGui::GetIO().KeyMods;
                const bool toggleSelection = (keyMods & ImGuiMod_Ctrl) != 0;
                const bool additiveSelection = !toggleSelection &&
                    (keyMods & ImGuiMod_Shift) != 0;
                const bool selectionOnly = toggleSelection || additiveSelection;
                const auto hoveredIt = std::find_if(
                    snapshot.clips.begin(), snapshot.clips.end(),
                    [&](const jcut::EditorClip& clip) {
                        return clip.id == hoveredClipId;
                    });
                const bool preserveSelectedGroup = !selectionOnly &&
                    hoveredMode == TimelineDragMode::MoveClip &&
                    hoveredIt != snapshot.clips.end() && hoveredIt->selected &&
                    selectedClipCount(snapshot) > 1;
                if (!selectionOnly && hoveredMode != TimelineDragMode::Seek) {
                    beginRuntimeHistoryTransaction(shellState);
                }
                applyCommand(shellState, jcut::SelectTrackCommand{hoveredTrackId});
                if (!preserveSelectedGroup) {
                    applyCommand(shellState, jcut::SelectClipCommand{
                        hoveredClipId, additiveSelection, toggleSelection});
                }
                shellState->timelineDragMode = selectionOnly
                    ? TimelineDragMode::None
                    : hoveredMode;
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

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) &&
        mouseInsideCanvas && hoveredRenderSyncMarker &&
        hoveredRenderSyncClipId != 0) {
        const auto ownerIt = std::find_if(
            snapshot.clips.begin(), snapshot.clips.end(),
            [&](const jcut::EditorClip& clip) {
                return clip.id == hoveredRenderSyncClipId;
            });
        if (ownerIt != snapshot.clips.end()) {
            if (!ownerIt->selected) {
                applyCommand(
                    shellState,
                    jcut::SelectTrackCommand{hoveredRenderSyncTrackId});
                applyCommand(
                    shellState,
                    jcut::SelectClipCommand{hoveredRenderSyncClipId});
            }
            applyCommand(shellState, jcut::SeekToFrameCommand{
                static_cast<int>(std::clamp<std::int64_t>(
                    hoveredRenderSyncMarker->frame,
                    0,
                    std::numeric_limits<int>::max()))});
            shellState->timelineContextClipId = ownerIt->id;
            shellState->timelineContextClipPersistentId =
                ownerIt->persistentId;
            shellState->timelineContextFrame =
                hoveredRenderSyncMarker->frame;
            shellState->timelineContextDocumentGeneration =
                shellState->documentGeneration;
            ImGui::OpenPopup("TimelineSyncMarkerContext");
        }
    } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) &&
               mouseInsideCanvas && hoveredClipId != 0) {
        const auto hoveredIt = std::find_if(
            snapshot.clips.begin(), snapshot.clips.end(),
            [&](const jcut::EditorClip& clip) {
                return clip.id == hoveredClipId;
            });
        if (hoveredIt != snapshot.clips.end() && !hoveredIt->selected) {
            applyCommand(shellState, jcut::SelectTrackCommand{hoveredTrackId});
            applyCommand(shellState, jcut::SelectClipCommand{hoveredClipId});
        }
        shellState->timelineContextClipId = hoveredClipId;
        shellState->timelineContextClipPersistentId =
            hoveredIt == snapshot.clips.end()
            ? std::string{}
            : hoveredIt->persistentId;
        shellState->timelineContextFrame = snapshot.transport.currentFrame;
        shellState->timelineContextDocumentGeneration =
            shellState->documentGeneration;
        ImGui::OpenPopup("TimelineClipContext");
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
                if (hoveredTrackIndex >= 0 &&
                    !jcut::isGeneratedEditorChildTrack(snapshot.tracks[
                        static_cast<std::size_t>(hoveredTrackIndex)])) {
                    targetTrackId = snapshot.tracks[hoveredTrackIndex].id;
                }
                const int unsnappedStart = static_cast<int>(std::clamp<std::int64_t>(
                    static_cast<std::int64_t>(shellState->timelineDragStartFrame) + deltaFrames,
                    0,
                    std::numeric_limits<int>::max()));
                const TimelineSnapResult snap = shellState->timelineSnappingEnabled
                    ? snapTimelineMoveStart(
                          snapshot, shellState->timelineDragClipId, unsnappedStart)
                    : TimelineSnapResult{unsnappedStart, -1};
                shellState->timelineSnapIndicatorFrame = snap.boundaryFrame;
                applyCommand(shellState, jcut::MoveSelectedClipsCommand{
                    shellState->timelineDragClipId,
                    targetTrackId,
                    snap.frame});
            } else if (shellState->timelineDragMode == TimelineDragMode::TrimClipStart) {
                const int maximumStart = static_cast<int>(
                    std::clamp<std::int64_t>(
                        static_cast<std::int64_t>(
                            shellState->timelineDragStartFrame) +
                            shellState->timelineDragDurationFrames - 1,
                        shellState->timelineDragStartFrame,
                        std::numeric_limits<int>::max()));
                const int unsnappedStart = static_cast<int>(std::clamp<std::int64_t>(
                    static_cast<std::int64_t>(shellState->timelineDragStartFrame) + deltaFrames,
                    0,
                    maximumStart));
                TimelineSnapResult snap = shellState->timelineSnappingEnabled
                    ? snapTimelineBoundary(
                          snapshot, unsnappedStart, shellState->timelineDragClipId)
                    : TimelineSnapResult{unsnappedStart, -1};
                snap.frame = std::clamp(snap.frame, 0, maximumStart);
                if (snap.frame != snap.boundaryFrame) {
                    snap.boundaryFrame = -1;
                }
                shellState->timelineSnapIndicatorFrame = snap.boundaryFrame;
                applyCommand(shellState, jcut::TrimClipStartCommand{
                    shellState->timelineDragClipId,
                    snap.frame});
            } else if (shellState->timelineDragMode == TimelineDragMode::TrimClipEnd) {
                const int minimumEnd = static_cast<int>(
                    std::min<std::int64_t>(
                        static_cast<std::int64_t>(
                            shellState->timelineDragStartFrame) + 1,
                        std::numeric_limits<int>::max()));
                const int unsnappedEnd = static_cast<int>(std::clamp<std::int64_t>(
                    static_cast<std::int64_t>(shellState->timelineDragStartFrame) +
                        shellState->timelineDragDurationFrames + deltaFrames,
                    minimumEnd,
                    std::numeric_limits<int>::max()));
                TimelineSnapResult snap = shellState->timelineSnappingEnabled
                    ? snapTimelineBoundary(
                          snapshot, unsnappedEnd, shellState->timelineDragClipId)
                    : TimelineSnapResult{unsnappedEnd, -1};
                snap.frame = std::max(minimumEnd, snap.frame);
                if (snap.frame != snap.boundaryFrame) {
                    snap.boundaryFrame = -1;
                }
                shellState->timelineSnapIndicatorFrame = snap.boundaryFrame;
                applyCommand(shellState, jcut::TrimClipEndCommand{
                    shellState->timelineDragClipId,
                    snap.frame});
            }
        }
    }
    if (shellState->timelineDragMode != TimelineDragMode::None && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        clearTimelineDrag(shellState);
    }

    if (ImGui::BeginPopup("TimelineSyncMarkerContext")) {
        const auto contextIt = std::find_if(
            snapshot.clips.begin(), snapshot.clips.end(),
            [&](const jcut::EditorClip& clip) {
                return shellState->timelineContextDocumentGeneration ==
                           shellState->documentGeneration &&
                    clip.id == shellState->timelineContextClipId &&
                    clip.persistentId ==
                        shellState->timelineContextClipPersistentId;
            });
        if (contextIt == snapshot.clips.end()) {
            shellState->statusMessage =
                "render sync menu closed after document change";
            ImGui::CloseCurrentPopup();
        } else {
            drawRenderSyncContextActions(
                shellState,
                snapshot,
                *contextIt,
                shellState->timelineContextFrame);
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopup("TimelineClipContext")) {
        const auto contextIt = std::find_if(
            snapshot.clips.begin(), snapshot.clips.end(),
            [&](const jcut::EditorClip& clip) {
                return shellState->timelineContextDocumentGeneration ==
                           shellState->documentGeneration &&
                    clip.id == shellState->timelineContextClipId &&
                    clip.persistentId ==
                        shellState->timelineContextClipPersistentId;
            });
        const jcut::EditorClip* contextClip = contextIt == snapshot.clips.end()
            ? nullptr
            : &*contextIt;
        if (!contextClip) {
            shellState->statusMessage =
                "clip menu closed after document change";
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::MenuItem("Cut", "Ctrl+X", false, contextClip != nullptr)) {
            applyCommand(shellState, jcut::CutSelectedClipsCommand{});
        }
        if (ImGui::MenuItem("Copy", "Ctrl+C", false, contextClip != nullptr)) {
            applyCommand(shellState, jcut::CopySelectedClipsCommand{});
        }
        if (ImGui::MenuItem("Paste At Playhead", "Ctrl+V")) {
            applyCommand(shellState, jcut::PasteClipsCommand{
                snapshot.transport.currentFrame, selectedTrackId(snapshot)});
        }
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false,
                            contextClip != nullptr)) {
            applyCommand(shellState, jcut::DuplicateSelectedClipsCommand{});
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Nudge Left", "Alt+Left", false,
                            contextClip && !contextClip->locked &&
                                contextClip->startFrame > 0)) {
            applyCommand(shellState, jcut::NudgeSelectedClipCommand{-1});
        }
        if (ImGui::MenuItem("Nudge Right", "Alt+Right", false,
                            contextClip && !contextClip->locked)) {
            applyCommand(shellState, jcut::NudgeSelectedClipCommand{1});
        }
        ImGui::Separator();
        const bool canSplit = contextClip &&
            !contextClip->locked &&
            snapshot.transport.currentFrame > contextClip->startFrame &&
            snapshot.transport.currentFrame <
                contextClip->startFrame + contextClip->durationFrames;
        if (ImGui::MenuItem("Split At Playhead", "Ctrl+B", false, canSplit)) {
            applyCommand(shellState, jcut::SplitClipCommand{
                contextClip->id, snapshot.transport.currentFrame});
        }
        if (ImGui::BeginMenu("Playback Speed",
                             contextClip && !contextClip->locked)) {
            constexpr std::array<double, 9> playbackRates = {
                0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0, 4.0};
            for (const double playbackRate : playbackRates) {
                char label[32];
                if (std::abs(playbackRate - 1.0) <= 0.0001) {
                    std::snprintf(label, sizeof(label), "1x (Normal)");
                } else {
                    std::snprintf(label, sizeof(label), "%.3gx", playbackRate);
                }
                const bool selectedRate = contextClip &&
                    std::abs(contextClip->playbackRate - playbackRate) <= 0.0001;
                if (ImGui::MenuItem(label, nullptr, selectedRate) &&
                    contextClip) {
                    applyCommand(shellState, jcut::SetClipPlaybackRateCommand{
                        contextClip->id, playbackRate});
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Scale to Fill Preview", nullptr, false,
                            contextClip && clipCanScaleToFill(*contextClip)) &&
            contextClip) {
            scaleClipToFillPreview(shellState, snapshot, *contextClip);
        }
        if (ImGui::BeginMenu("Render Sync", contextClip != nullptr)) {
            if (contextClip) {
                drawRenderSyncContextActions(
                    shellState,
                    snapshot,
                    *contextClip,
                    shellState->timelineContextFrame);
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Reset Grading", nullptr, false,
                            contextClip != nullptr) && contextClip) {
            applyCommand(shellState,
                         jcut::ResetClipGradingCommand{contextClip->id});
        }
        if (ImGui::MenuItem("Delete Selected", "Delete", false,
                            contextClip && !contextClip->locked)) {
            deleteSelectedClips(shellState);
        }
        if (contextClip) {
            ImGui::Separator();
            if (ImGui::MenuItem(contextClip->locked ? "Unlock" : "Lock")) {
                applyCommand(shellState, jcut::SetClipLockedCommand{
                    contextClip->id, !contextClip->locked});
            }
        }
        ImGui::EndPopup();
    }

    RenderSyncMarkerDraft& renderSyncDraft =
        shellState->renderSyncMarkerDraft;
    if (std::exchange(renderSyncDraft.popupRequested, false)) {
        ImGui::OpenPopup("Render Sync Count");
    }
    if (ImGui::BeginPopupModal(
            "Render Sync Count", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const auto draftClipIt = std::find_if(
            snapshot.clips.begin(), snapshot.clips.end(),
            [&](const jcut::EditorClip& clip) {
                return renderSyncDraft.documentGeneration ==
                           shellState->documentGeneration &&
                    clip.id == renderSyncDraft.clipId &&
                    clip.persistentId == renderSyncDraft.clipPersistentId;
            });
        if (draftClipIt == snapshot.clips.end()) {
            shellState->statusMessage =
                "render sync edit canceled after document change";
            ImGui::CloseCurrentPopup();
        } else {
            ImGui::TextUnformatted(renderSyncDraft.skipFrame
                ? "How many frames should be skipped for this clip?"
                : "How many extra frames should be duplicated for this clip?");
            ImGui::SetNextItemWidth(160.0f);
            const bool submitFromKeyboard = ImGui::InputInt(
                "Count",
                &renderSyncDraft.count,
                1,
                10,
                ImGuiInputTextFlags_EnterReturnsTrue);
            renderSyncDraft.count = std::clamp(
                renderSyncDraft.count,
                jcut::kEditorRenderSyncMinCount,
                jcut::kEditorRenderSyncMaxCount);
            if (ImGui::Button("Apply") || submitFromKeyboard) {
                applyCommand(shellState, jcut::AddRenderSyncMarkerCommand{
                    draftClipIt->id,
                    renderSyncDraft.frame,
                    renderSyncDraft.skipFrame,
                    renderSyncDraft.count});
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }

    ImGui::InvisibleButton("TimelineCanvas", avail);
    if (ImGui::BeginDragDropTarget()) {
        const TimelineTrackDropTarget dropTarget = timelineTrackDropTarget(
            snapshot, origin.y, ImGui::GetIO().MousePos.y);
        int dropFrame = frameFromTimelineX(origin.x, ImGui::GetIO().MousePos.x);
        if (shellState->timelineSnappingEnabled) {
            const TimelineSnapResult snap = snapTimelineBoundary(snapshot, dropFrame);
            dropFrame = snap.frame;
            shellState->timelineSnapIndicatorFrame = snap.boundaryFrame;
        }

        auto payloadText = [](const ImGuiPayload& payload) {
            if (!payload.Data || payload.DataSize <= 1) {
                return std::string{};
            }
            const char* bytes = static_cast<const char*>(payload.Data);
            std::size_t size = static_cast<std::size_t>(payload.DataSize);
            if (bytes[size - 1] != '\0') {
                return std::string{};
            }
            return std::string(bytes, size - 1);
        };

        constexpr ImGuiDragDropFlags acceptFlags =
            ImGuiDragDropFlags_AcceptBeforeDelivery;
        const ImGuiPayload* projectPayload = ImGui::AcceptDragDropPayload(
            kProjectMediaDragPayload, acceptFlags);
        const ImGuiPayload* filesystemPayload = ImGui::AcceptDragDropPayload(
            kFilesystemMediaDragPayload, acceptFlags);
        if (projectPayload || filesystemPayload) {
            const float dropX = origin.x + kTimelineLabelWidth +
                kTimelineTrackPadding +
                static_cast<float>(dropFrame) * kTimelinePixelsPerFrame;
            const float trackY = origin.y + kTimelineTopPadding +
                static_cast<float>(dropTarget.trackIndex) * kTimelineRowHeight;
            if (dropTarget.insertTrack) {
                drawList->AddLine(
                    ImVec2(origin.x + 4.0f, trackY),
                    ImVec2(origin.x + avail.x - 12.0f, trackY),
                    IM_COL32(92, 232, 178, 255),
                    3.0f);
            } else {
                drawList->AddRect(
                    ImVec2(origin.x + kTimelineLabelWidth, trackY),
                    ImVec2(origin.x + avail.x - 12.0f,
                           trackY + kTimelineClipHeight),
                    IM_COL32(92, 232, 178, 230),
                    4.0f,
                    0,
                    2.0f);
            }
            drawList->AddLine(
                ImVec2(dropX, trackY - 3.0f),
                ImVec2(dropX, trackY + kTimelineClipHeight + 3.0f),
                IM_COL32(92, 232, 178, 255),
                3.0f);
        }
        if (projectPayload && projectPayload->IsDelivery()) {
            const std::string mediaId = payloadText(*projectPayload);
            const auto mediaItem = std::find_if(
                snapshot.mediaItems.begin(), snapshot.mediaItems.end(),
                [&](const jcut::EditorMediaItem& item) {
                    return item.id == mediaId;
                });
            if (mediaItem != snapshot.mediaItems.end()) {
                std::int64_t probedDurationFrames = 0;
                const jcut::ImportMediaCommand probedMedia =
                    importMediaCommandForPath(
                        mediaId,
                        mediaItem->label,
                        mediaItem->kind,
                        &probedDurationFrames);
                const int durationFrames = resolvedMediaDurationFrames(
                    0, probedDurationFrames);
                const std::string dropMediaKind = probedMedia.mediaKind;
                insertDroppedMedia(
                    shellState,
                    snapshot,
                    dropTarget,
                    dropMediaKind,
                    dropFrame,
                    durationFrames,
                    [&](int trackId) {
                        applyCommand(shellState, jcut::InsertClipFromMediaCommand{
                            mediaId, trackId, dropFrame, durationFrames});
                    });
            } else {
                shellState->statusMessage = "dropped project media is unavailable";
            }
        }
        if (filesystemPayload && filesystemPayload->IsDelivery()) {
            const fs::path mediaPath(payloadText(*filesystemPayload));
            if (isImportableMediaPath(mediaPath)) {
                const jcut::AddClipCommand addClip = addClipCommandForPath(
                    mediaPath, 0, dropFrame);
                insertDroppedMedia(
                    shellState,
                    snapshot,
                    dropTarget,
                    addClip.mediaKind,
                    dropFrame,
                    addClip.durationFrames,
                    [&](int trackId) {
                        jcut::AddClipCommand routed = addClip;
                        routed.trackId = trackId;
                        applyCommand(shellState, std::move(routed));
                    });
            } else {
                shellState->statusMessage = "dropped media is unavailable";
            }
        }
        ImGui::EndDragDropTarget();
    }

    ImGui::Separator();
    for (const jcut::EditorTrack& track : snapshot.tracks) {
        if (ImGui::Selectable(track.label.c_str(), track.selected)) {
            applyCommand(shellState, jcut::SelectTrackCommand{track.id});
        }
    }
    for (const jcut::EditorClip& clip : snapshot.clips) {
        if (ImGui::Selectable(clip.label.c_str(), clip.selected)) {
            const ImGuiKeyChord keyMods = ImGui::GetIO().KeyMods;
            const bool toggleSelection = (keyMods & ImGuiMod_Ctrl) != 0;
            const bool additiveSelection = !toggleSelection &&
                (keyMods & ImGuiMod_Shift) != 0;
            applyCommand(shellState, jcut::SelectClipCommand{
                clip.id, additiveSelection, toggleSelection});
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

jcut::TranscriptSourceSpec transcriptSourceSpecForClip(
    const ShellState& shellState,
    const jcut::EditorClip& clip)
{
    jcut::TranscriptSourceSpec source;
    source.sourcePath = clip.sourcePath;
    source.audioSourcePath = clip.audioSourcePath;
    source.audioSourceMode = clip.audioSourceMode;
    source.audioSourceStatus = clip.audioSourceStatus;
    source.audioStreamIndex = clip.audioStreamIndex;
    source.sourceRootPath = shellState.mediaRootDirectory.empty()
        ? shellState.projectRootPath
        : shellState.mediaRootDirectory;

    if (!source.audioSourcePath.empty() &&
        source.audioSourceStatus != "ok" &&
        source.audioSourceMode == "sidecar") {
        fs::path audioPath(source.audioSourcePath);
        if (audioPath.is_relative() && !source.sourceRootPath.empty()) {
            audioPath = fs::path(source.sourceRootPath) / audioPath;
        }
        std::error_code ec;
        if (fs::is_regular_file(audioPath, ec) && !ec) {
            source.audioSourceStatus = "ok";
        }
    }
    return source;
}

std::string requestedTranscriptCutPath(const ShellState& shellState,
                                       const jcut::EditorClip& clip)
{
    return clip.transcriptActiveCutPath.empty()
        ? legacyStringValue(shellState, "transcriptActiveCutPath")
        : clip.transcriptActiveCutPath;
}

void ensureTranscriptInspectorCache(
    ShellState* shellState,
    const jcut::EditorDocumentCore& snapshot,
    const jcut::EditorClip& clip)
{
    TranscriptInspectorCache& cache = shellState->transcriptCache;
    const jcut::TranscriptSourceSpec source =
        transcriptSourceSpecForClip(*shellState, clip);
    const jcut::TranscriptSourceIdentity identity =
        jcut::resolveTranscriptSourceIdentity(source);
    const std::string requestedPath = requestedTranscriptCutPath(*shellState, clip);
    jcut::TranscriptTiming timing;
    timing.framesPerSecond =
        std::isfinite(snapshot.exportRequest.outputFps) &&
            snapshot.exportRequest.outputFps > 0.0
        ? snapshot.exportRequest.outputFps
        : 30.0;
    timing.prependMilliseconds = snapshot.exportRequest.transcriptPrependMs;
    timing.postpendMilliseconds = snapshot.exportRequest.transcriptPostpendMs;
    timing.offsetMilliseconds = snapshot.exportRequest.transcriptOffsetMs;
    const bool includeOutsideCut = legacyBoolValue(
        *shellState, "transcriptShowExcludedLines", false);

    const bool keyChanged = cache.clipId != clip.id ||
        cache.sourceKey != identity.canonicalKey ||
        cache.requestedPath != requestedPath ||
        cache.includeOutsideCut != includeOutsideCut ||
        cache.timing.framesPerSecond != timing.framesPerSecond ||
        cache.timing.prependMilliseconds != timing.prependMilliseconds ||
        cache.timing.postpendMilliseconds != timing.postpendMilliseconds ||
        cache.timing.offsetMilliseconds != timing.offsetMilliseconds;
    const auto now = std::chrono::steady_clock::now();
    bool filesChanged = false;
    if (cache.loaded && !keyChanged && !cache.refreshRequested &&
        now >= cache.nextFilesystemCheck) {
        cache.nextFilesystemCheck = now + std::chrono::milliseconds(500);
        filesChanged =
            jcut::transcriptCatalogDirectoryWriteTime(identity) !=
                cache.session.catalogDirectoryWriteTimeTicks ||
            jcut::inspectTranscriptFile(cache.session.activePath) !=
                cache.session.activeStamp ||
            jcut::inspectTranscriptFile(cache.session.catalog.originalPath) !=
                cache.session.originalStamp ||
            cache.session.catalog.cuts.size() != cache.session.cutStamps.size();
        if (!filesChanged) {
            for (std::size_t index = 0;
                 index < cache.session.catalog.cuts.size();
                 ++index) {
                if (jcut::inspectTranscriptFile(
                        cache.session.catalog.cuts[index].path) !=
                    cache.session.cutStamps[index]) {
                    filesChanged = true;
                    break;
                }
            }
        }
    }
    if (cache.loaded && !cache.refreshRequested && !keyChanged && !filesChanged) {
        return;
    }
    if (keyChanged || filesChanged) {
        cache.miningProposals.clear();
        cache.miningProposalSelected.clear();
        cache.miningProposalLabel.clear();
    }

    jcut::TranscriptCutSessionOptions options;
    options.requestedActivePath = requestedPath;
    options.timing = timing;
    options.includeOutsideActiveCut = includeOutsideCut;
    options.ensureEditable = true;
    cache.session = jcut::loadTranscriptCutSession(source, options);
    cache.clipId = clip.id;
    cache.sourceKey = identity.canonicalKey;
    cache.requestedPath = requestedPath;
    cache.timing = timing;
    cache.includeOutsideCut = includeOutsideCut;
    cache.loaded = true;
    cache.refreshRequested = false;
    cache.nextFilesystemCheck = now + std::chrono::milliseconds(500);
}

std::string transcriptRowEditLabels(const jcut::TranscriptRow& row)
{
    std::vector<std::string> labels;
    if ((row.editFlags & jcut::TranscriptEditTiming) != 0U) {
        labels.emplace_back("Timing");
    }
    if ((row.editFlags & jcut::TranscriptEditText) != 0U) {
        labels.emplace_back("Text");
    }
    if ((row.editFlags & jcut::TranscriptEditSkip) != 0U) {
        labels.emplace_back("Skip");
    }
    if ((row.editFlags & jcut::TranscriptEditInserted) != 0U) {
        labels.emplace_back("Inserted");
    }
    if (row.skipped) {
        labels.emplace_back("Skipped");
    }
    if (row.gap) {
        labels.emplace_back("Gap");
    }
    if (row.outsideActiveCut) {
        labels.emplace_back("Outside Cut");
    }
    std::string result;
    for (const std::string& label : labels) {
        if (!result.empty()) {
            result += ", ";
        }
        result += label;
    }
    return result.empty() ? std::string("None") : result;
}

void selectTranscriptWordDraft(TranscriptInspectorCache* cache,
                               const jcut::TranscriptRow& row)
{
    if (!cache || row.gap || row.outsideActiveCut || row.word.segmentIndex < 0 ||
        row.word.wordIndex < 0) {
        return;
    }
    cache->selectedWord = row.word;
    cache->selectedText = row.text;
    cache->selectedStartSeconds = row.rawStartSeconds;
    cache->selectedEndSeconds = row.rawEndSeconds;
    cache->selectedSkipped = row.skipped;
    cache->selectionDraftValid = true;
    cache->mutationError.clear();
}

bool saveTranscriptMutation(ShellState* shellState,
                            TranscriptInspectorCache* cache,
                            nlohmann::json root)
{
    if (!shellState || !cache || !cache->session.activeDocument ||
        cache->session.activePath.empty()) return false;
    const std::string path = cache->session.activePath;
    const std::string previousPayload =
        cache->session.activeDocument->root().dump();
    const std::string nextPayload = root.dump();
    if (previousPayload == nextPayload) return false;

    const jcut::TranscriptFileStamp currentStamp =
        jcut::inspectTranscriptFile(path);
    const auto expected =
        shellState->transcriptHistoryExpectedStamps.find(path);
    const bool externallyChanged =
        expected != shellState->transcriptHistoryExpectedStamps.end() &&
        currentStamp != expected->second;
    std::string error;
    if (!jcut::saveTranscriptDocumentAtomic(path, root, &error)) {
        cache->mutationError = std::move(error);
        return false;
    }

    jcut::CommandResult updateResult;
    {
        std::lock_guard<std::mutex> lock(shellState->runtimeMutex);
        if (externallyChanged) shellState->runtime.clearHistory();
        (void)shellState->runtime.execute(jcut::EditorCommand{
            jcut::SeedTranscriptHistoryDocumentCommand{
                path, previousPayload}});
        updateResult = shellState->runtime.execute(jcut::EditorCommand{
            jcut::SetTranscriptHistoryDocumentCommand{
                path, nextPayload}});
    }
    if (!updateResult.applied) {
        std::string rollbackError;
        const nlohmann::json previousRoot =
            nlohmann::json::parse(previousPayload);
        (void)jcut::saveTranscriptDocumentAtomic(
            path, previousRoot, &rollbackError);
        cache->mutationError = updateResult.message;
        if (!rollbackError.empty()) {
            cache->mutationError += "; rollback failed: " + rollbackError;
        }
        return false;
    }
    shellState->transcriptHistoryExpectedStamps[path] =
        jcut::inspectTranscriptFile(path);
    shellState->statusMessage = externallyChanged
        ? "Transcript updated; prior undo history was cleared after an external file change."
        : updateResult.message;
    cache->mutationError.clear();
    cache->selectionDraftValid = false;
    cache->refreshRequested = true;
    requestPreviewRender(shellState);
    return true;
}

void drawReadOnlyTableRow(const char* key, const std::string& value)
{
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(key);
    ImGui::TableNextColumn();
    ImGui::TextWrapped("%s", value.c_str());
}

std::string effectPresetDisplayName(std::string_view presetId)
{
    if (presetId == "none") {
        return "Off";
    }
    std::string label(presetId);
    bool capitalize = true;
    for (char& value : label) {
        if (value == '_') {
            value = ' ';
            capitalize = true;
        } else if (capitalize && value >= 'a' && value <= 'z') {
            value = static_cast<char>(value - 'a' + 'A');
            capitalize = false;
        } else {
            capitalize = false;
        }
    }
    return label;
}

bool effectPresetIsSpeakerMask(std::string_view presetId)
{
    return presetId == "speaker_mask_dilation" ||
        presetId == "speaker_mask_dilation_pulse" ||
        presetId == "speaker_mask_dilation_rings";
}

bool effectPresetUsesCommonNeutralParameters(std::string_view presetId)
{
    return presetId == "news_logo_ticker" ||
        presetId == "person_orbit" ||
        presetId == "alternating_motion_background" ||
        presetId == "freeze_pattern" ||
        presetId == "step_repeat" ||
        presetId == "directional_trim_ticker" ||
        presetId == "source_tile" ||
        presetId == "vulkan_3d_synth" ||
        presetId == "progressive_edge_stretch" ||
        presetId == "sobel_edges" ||
        presetId == "neon_glow" ||
        effectPresetIsSpeakerMask(presetId);
}

bool effectPresetUsesTilingParameters(std::string_view presetId)
{
    return presetId == "source_tile" ||
        presetId == "tessellation" ||
        presetId == "hexagonal_prism" ||
        effectPresetIsSpeakerMask(presetId);
}

bool drawFrameSeekCell(ShellState* shellState,
                       std::int64_t displayedFrame,
                       std::int64_t timelineFrame,
                       const std::string& id)
{
    const std::string label = std::to_string(displayedFrame) + "##" + id;
    if (!ImGui::Selectable(label.c_str())) {
        return false;
    }
    const std::int64_t clampedFrame = std::clamp<std::int64_t>(
        timelineFrame,
        0,
        std::numeric_limits<int>::max());
    applyCommand(shellState,
                 jcut::SeekToFrameCommand{static_cast<int>(clampedFrame)});
    return true;
}

void replaceRenderSyncMarker(
    ShellState* shellState,
    const jcut::EditorRenderSyncMarker& marker,
    int numericClipId,
    std::int64_t frame,
    bool skipFrame,
    int count)
{
    beginRuntimeHistoryTransaction(shellState);
    const jcut::CommandResult removed = applyCommand(
        shellState,
        jcut::RemoveRenderSyncMarkerCommand{
            marker.clipId, marker.frame, marker.skipFrame});
    if (removed.applied) {
        applyCommand(shellState,
                     jcut::AddRenderSyncMarkerCommand{
                         numericClipId,
                         std::max<std::int64_t>(0, frame),
                         skipFrame,
                         std::clamp(
                             count,
                             jcut::kEditorRenderSyncMinCount,
                             jcut::kEditorRenderSyncMaxCount)});
    }
    endRuntimeHistoryTransaction(shellState);
}

void hydrateTitleDraft(ShellState* shellState,
                       int clipId,
                       const jcut::EditorTitleKeyframe& keyframe)
{
    shellState->titleDraftClipId = clipId;
    shellState->titleDraft = keyframe;
}

template <std::size_t Capacity>
bool inputTextForString(const char* label, std::string* value)
{
    std::array<char, Capacity> buffer{};
    const std::size_t textLength = std::min(value->size(), buffer.size() - 1);
    std::memcpy(buffer.data(), value->data(), textLength);
    if (!ImGui::InputText(label, buffer.data(), buffer.size())) {
        return false;
    }
    *value = buffer.data();
    return true;
}

template <typename Keyframe>
Keyframe& ensureKeyframeDraft(ShellState* shellState,
                              int clipId,
                              jcut::EditorKeyframeChannel channel,
                              Keyframe initial)
{
    InspectorKeyframeDraft& draft = shellState->keyframeDraft;
    if (draft.clipId != clipId || draft.channel != channel ||
        !std::holds_alternative<Keyframe>(draft.value)) {
        draft.clipId = clipId;
        draft.channel = channel;
        draft.originalFrame = initial.frame;
        draft.existing = false;
        draft.value = std::move(initial);
    }
    return std::get<Keyframe>(draft.value);
}

template <typename Keyframe>
void loadKeyframeDraft(ShellState* shellState,
                       int clipId,
                       jcut::EditorKeyframeChannel channel,
                       const Keyframe& keyframe)
{
    shellState->keyframeDraft = {
        clipId, channel, keyframe.frame, true, keyframe};
}

template <typename Keyframe, typename UpsertCommand>
void commitKeyframeDraft(ShellState* shellState,
                         int clipId,
                         Keyframe* keyframe)
{
    InspectorKeyframeDraft& draft = shellState->keyframeDraft;
    jcut::CommandResult result;
    if (draft.existing && draft.originalFrame != keyframe->frame) {
        beginRuntimeHistoryTransaction(shellState);
        const jcut::CommandResult removed = applyCommand(
            shellState,
            jcut::RemoveClipKeyframeCommand{
                clipId, draft.channel, draft.originalFrame});
        if (removed.applied) {
            result = applyCommand(
                shellState, UpsertCommand{clipId, *keyframe});
        } else {
            result = removed;
        }
        endRuntimeHistoryTransaction(shellState);
    } else {
        result = applyCommand(
            shellState, UpsertCommand{clipId, *keyframe});
    }
    if (result.applied) {
        draft.originalFrame = keyframe->frame;
        draft.existing = true;
    }
}

void removeInspectorKeyframe(ShellState* shellState,
                             int clipId,
                             jcut::EditorKeyframeChannel channel,
                             std::int64_t frame)
{
    const jcut::CommandResult result = applyCommand(
        shellState,
        jcut::RemoveClipKeyframeCommand{clipId, channel, frame});
    const InspectorKeyframeDraft& draft = shellState->keyframeDraft;
    if (result.applied && draft.existing && draft.clipId == clipId &&
        draft.channel == channel && draft.originalFrame == frame) {
        shellState->keyframeDraft = {};
    }
}

template <typename Keyframe, typename UpsertCommand, typename DrawFields>
void drawKeyframeDraftEditor(ShellState* shellState,
                             const jcut::EditorClip* clip,
                             jcut::EditorKeyframeChannel channel,
                             std::int64_t lastFrame,
                             const char* editorId,
                             Keyframe initial,
                             DrawFields&& drawFields)
{
    if (!clip) {
        return;
    }
    Keyframe& keyframe = ensureKeyframeDraft(
        shellState, clip->id, channel, initial);
    InspectorKeyframeDraft& draft = shellState->keyframeDraft;

    ImGui::PushID(editorId);
    ImGui::Separator();
    ImGui::TextUnformatted(draft.existing ? "Edit Keyframe" : "New Keyframe");
    if (draft.existing) {
        ImGui::SameLine();
        ImGui::TextDisabled("(loaded from frame %lld)",
                            static_cast<long long>(draft.originalFrame));
    }
    int frame = static_cast<int>(std::clamp<std::int64_t>(
        keyframe.frame, 0, std::min<std::int64_t>(
            lastFrame, std::numeric_limits<int>::max())));
    ImGui::InputInt("Frame", &frame);
    keyframe.frame = std::clamp<std::int64_t>(frame, 0, lastFrame);
    drawFields(&keyframe);
    ImGui::Checkbox("Linear Interpolation", &keyframe.linearInterpolation);
    if (ImGui::Button(draft.existing ? "Apply Edit" : "Add Keyframe")) {
        commitKeyframeDraft<Keyframe, UpsertCommand>(
            shellState, clip->id, &keyframe);
    }
    ImGui::SameLine();
    if (ImGui::Button("New At Playhead")) {
        loadKeyframeDraft(shellState, clip->id, channel, initial);
        shellState->keyframeDraft.existing = false;
    }
    ImGui::Separator();
    ImGui::PopID();
}

bool drawGradingCurvePointEditor(const char* channelLabel,
                                 std::vector<jcut::EditorPoint>* points,
                                 bool editsDisabled)
{
    if (!points) {
        return false;
    }
    *points = jcut::sanitizeEditorGradingCurve(*points);

    bool curveChanged = false;
    ImGui::PushID(channelLabel);
    if (ImGui::TreeNode(channelLabel)) {
        if (editsDisabled) {
            ImGui::TextDisabled(
                "RGB points follow Lift / Gamma / Gain while three-point lock is enabled");
        }
        ImGui::BeginDisabled(editsDisabled);

        constexpr double kCurveXMinimum = 0.0;
        constexpr double kCurveXMaximum = 1.0;
        constexpr double kCurveYMinimum = -1.0;
        constexpr double kCurveYMaximum = 2.0;
        std::optional<std::size_t> pointToRemove;
        if (ImGui::BeginTable(
                "CurvePointTable",
                3,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                    ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("X");
            ImGui::TableSetupColumn("Y");
            ImGui::TableSetupColumn("Action");
            ImGui::TableHeadersRow();
            for (std::size_t pointIndex = 0; pointIndex < points->size(); ++pointIndex) {
                jcut::EditorPoint& point = points->at(pointIndex);
                const bool endpoint = pointIndex == 0 || pointIndex + 1 == points->size();
                ImGui::PushID(static_cast<int>(pointIndex));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::BeginDisabled(endpoint);
                curveChanged |= ImGui::SliderScalar(
                    "##X",
                    ImGuiDataType_Double,
                    &point.x,
                    &kCurveXMinimum,
                    &kCurveXMaximum,
                    "%.3f");
                ImGui::EndDisabled();
                if (endpoint) {
                    point.x = pointIndex == 0 ? kCurveXMinimum : kCurveXMaximum;
                }
                ImGui::TableNextColumn();
                curveChanged |= ImGui::SliderScalar(
                    "##Y",
                    ImGuiDataType_Double,
                    &point.y,
                    &kCurveYMinimum,
                    &kCurveYMaximum,
                    "%.3f");
                ImGui::TableNextColumn();
                if (endpoint) {
                    ImGui::TextDisabled("Fixed X");
                } else if (ImGui::SmallButton("Remove")) {
                    pointToRemove = pointIndex;
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        if (pointToRemove.has_value()) {
            points->erase(points->begin() + static_cast<std::ptrdiff_t>(*pointToRemove));
            curveChanged = true;
        }
        if (ImGui::SmallButton("Add point")) {
            std::size_t insertionIndex = 1;
            double widestGap = -1.0;
            for (std::size_t pointIndex = 1; pointIndex < points->size(); ++pointIndex) {
                const double gap = points->at(pointIndex).x - points->at(pointIndex - 1).x;
                if (gap > widestGap) {
                    widestGap = gap;
                    insertionIndex = pointIndex;
                }
            }
            const jcut::EditorPoint& previous = points->at(insertionIndex - 1);
            const jcut::EditorPoint& next = points->at(insertionIndex);
            points->insert(
                points->begin() + static_cast<std::ptrdiff_t>(insertionIndex),
                jcut::EditorPoint{
                    (previous.x + next.x) * 0.5,
                    (previous.y + next.y) * 0.5});
            curveChanged = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset channel")) {
            *points = {{0.0, 0.0}, {1.0, 1.0}};
            curveChanged = true;
        }
        if (curveChanged) {
            *points = jcut::sanitizeEditorGradingCurve(*points);
        }

        ImGui::EndDisabled();
        ImGui::TreePop();
    }
    ImGui::PopID();
    return curveChanged;
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
            const ImGuiKeyChord keyMods = ImGui::GetIO().KeyMods;
            const bool toggleSelection = (keyMods & ImGuiMod_Ctrl) != 0;
            const bool additiveSelection = !toggleSelection &&
                (keyMods & ImGuiMod_Shift) != 0;
            applyCommand(shellState, jcut::SelectClipCommand{
                clip.id, additiveSelection, toggleSelection});
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
    constexpr const char* kTrackVisualModeLabels[] = {
        "Enabled",
        "Force Opaque",
        "Hidden",
    };
    const jcut::EditorTrack* crossfadeTrack = selectedTrack(snapshot);
    const std::size_t crossfadeClipCount = crossfadeTrack
        ? static_cast<std::size_t>(std::count_if(
              snapshot.clips.begin(),
              snapshot.clips.end(),
              [&](const jcut::EditorClip& clip) {
                  return clip.trackId == crossfadeTrack->id;
              }))
        : 0;
    shellState->trackCrossfadeSeconds = std::clamp(
        shellState->trackCrossfadeSeconds, 0.01f, 30.0f);
    ImGui::SetNextItemWidth(120.0f);
    ImGui::DragFloat(
        "Crossfade (seconds)",
        &shellState->trackCrossfadeSeconds,
        0.01f,
        0.01f,
        30.0f,
        "%.2f");
    ImGui::SameLine();
    ImGui::Checkbox(
        "Move clips to overlap",
        &shellState->trackCrossfadeMoveClips);
    ImGui::SameLine();
    const bool crossfadeUnavailable = !crossfadeTrack ||
        jcut::isGeneratedEditorChildTrack(*crossfadeTrack) ||
        crossfadeClipCount < 2;
    ImGui::BeginDisabled(crossfadeUnavailable);
    if (ImGui::Button("Crossfade Consecutive Clips")) {
        applyCommand(shellState, jcut::CrossfadeTrackCommand{
            crossfadeTrack->id,
            static_cast<double>(shellState->trackCrossfadeSeconds),
            shellState->trackCrossfadeMoveClips});
    }
    ImGui::EndDisabled();
    if (crossfadeUnavailable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(
            crossfadeTrack && jcut::isGeneratedEditorChildTrack(*crossfadeTrack)
                ? "Generated child lanes cannot be crossfaded directly"
                : "Select an ordinary track containing at least two clips");
    }
    const ImGuiTableFlags tableFlags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX;
    if (!ImGui::BeginTable("TracksTable", 10, tableFlags)) {
        return;
    }
    ImGui::TableSetupColumn("Track", ImGuiTableColumnFlags_WidthFixed, 48.0f);
    ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 132.0f);
    ImGui::TableSetupColumn("Height", ImGuiTableColumnFlags_WidthFixed, 76.0f);
    ImGui::TableSetupColumn("Order", ImGuiTableColumnFlags_WidthFixed, 58.0f);
    ImGui::TableSetupColumn("Visual", ImGuiTableColumnFlags_WidthFixed, 110.0f);
    ImGui::TableSetupColumn("Grade", ImGuiTableColumnFlags_WidthFixed, 48.0f);
    ImGui::TableSetupColumn("Audio", ImGuiTableColumnFlags_WidthFixed, 48.0f);
    ImGui::TableSetupColumn("Gain", ImGuiTableColumnFlags_WidthFixed, 72.0f);
    ImGui::TableSetupColumn("Mute", ImGuiTableColumnFlags_WidthFixed, 44.0f);
    ImGui::TableSetupColumn("Solo", ImGuiTableColumnFlags_WidthFixed, 42.0f);
    ImGui::TableHeadersRow();
    for (std::size_t trackIndex = 0; trackIndex < snapshot.tracks.size(); ++trackIndex) {
        const jcut::EditorTrack& track = snapshot.tracks[trackIndex];
        const bool generatedChildTrack =
            jcut::isGeneratedEditorChildTrack(track);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        const std::string trackNumber = std::to_string(trackIndex + 1);
        if (ImGui::Selectable(trackNumber.c_str(), track.selected)) {
            applyCommand(shellState, jcut::SelectTrackCommand{track.id});
        }
        ImGui::PushID(track.id);
        ImGui::TableNextColumn();
        if (generatedChildTrack) {
            ImGui::TextUnformatted(track.label.c_str());
            const std::string generatedTrackIdentity =
                "source " +
                (track.parentClipId.empty() ? std::string("?") : track.parentClipId) +
                " -> child " +
                (track.childClipId.empty() ? std::string("?") : track.childClipId);
            ImGui::TextDisabled("%s", generatedTrackIdentity.c_str());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Derived Mask Matte lane (label is read-only)");
            }
        } else {
            std::string trackLabel = track.label;
            ImGui::SetNextItemWidth(126.0f);
            const bool labelChanged = ImGui::InputText("##trackLabel", &trackLabel);
            beginRuntimeHistoryTransactionForLastItem(shellState);
            if (labelChanged) {
                applyCommand(shellState, jcut::SetTrackPropertiesCommand{
                    track.id, std::move(trackLabel), track.height});
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Type to rename");
            }
        }
        ImGui::TableNextColumn();
        int trackHeight = track.height;
        const int maximumTrackHeight = generatedChildTrack
            ? 56
            : jcut::kEditorTrackMaxHeight;
        ImGui::SetNextItemWidth(70.0f);
        const bool heightChanged = ImGui::DragInt(
            "##trackHeight",
            &trackHeight,
            1.0f,
            jcut::kEditorTrackMinHeight,
            maximumTrackHeight,
            "%d px");
        beginRuntimeHistoryTransactionForLastItem(shellState);
        if (heightChanged) {
            applyCommand(shellState, jcut::SetTrackPropertiesCommand{
                track.id, track.label, trackHeight});
        }
        ImGui::TableNextColumn();
        const int previousOrdinaryTrack = adjacentOrdinaryTrackIndex(
            snapshot, static_cast<int>(trackIndex), -1);
        const int nextOrdinaryTrack = adjacentOrdinaryTrackIndex(
            snapshot, static_cast<int>(trackIndex), 1);
        ImGui::BeginDisabled(
            generatedChildTrack || previousOrdinaryTrack < 0);
        if (ImGui::ArrowButton("up", ImGuiDir_Up)) {
            applyCommand(shellState, jcut::ReorderTrackCommand{
                track.id, previousOrdinaryTrack});
        }
        ImGui::EndDisabled();
        ImGui::SameLine(0.0f, 2.0f);
        ImGui::BeginDisabled(generatedChildTrack || nextOrdinaryTrack < 0);
        if (ImGui::ArrowButton("down", ImGuiDir_Down)) {
            applyCommand(shellState, jcut::ReorderTrackCommand{
                track.id, nextOrdinaryTrack});
        }
        ImGui::EndDisabled();
        jcut::SetTrackStateCommand trackState{
            track.id,
            track.visualMode,
            track.audioEnabled,
            track.audioGain,
            track.audioMuted,
            track.audioSolo,
            track.gradingPreviewEnabled,
        };
        ImGui::TableNextColumn();
        int visualMode = std::clamp(trackState.visualMode, 0, 2);
        ImGui::SetNextItemWidth(104.0f);
        if (ImGui::Combo("##visualMode", &visualMode, kTrackVisualModeLabels,
                         IM_ARRAYSIZE(kTrackVisualModeLabels))) {
            trackState.visualMode = visualMode;
            applyCommand(shellState, trackState);
        }
        ImGui::TableNextColumn();
        bool gradingPreviewEnabled = trackState.gradingPreviewEnabled;
        if (ImGui::Checkbox("##gradingPreviewEnabled", &gradingPreviewEnabled)) {
            trackState.gradingPreviewEnabled = gradingPreviewEnabled;
            applyCommand(shellState, trackState);
        }
        ImGui::BeginDisabled(generatedChildTrack);
        ImGui::TableNextColumn();
        bool audioEnabled = trackState.audioEnabled;
        if (ImGui::Checkbox("##audioEnabled", &audioEnabled)) {
            trackState.audioEnabled = audioEnabled;
            applyCommand(shellState, trackState);
        }
        ImGui::TableNextColumn();
        float gain = static_cast<float>(trackState.audioGain);
        ImGui::SetNextItemWidth(66.0f);
        const bool gainChanged =
            ImGui::DragFloat("##audioGain", &gain, 0.01f, 0.0f, 8.0f, "%.2f");
        beginRuntimeHistoryTransactionForLastItem(shellState);
        if (gainChanged) {
            trackState.audioGain = gain;
            applyCommand(shellState, trackState);
        }
        ImGui::TableNextColumn();
        bool audioMuted = trackState.audioMuted;
        if (ImGui::Checkbox("##audioMuted", &audioMuted)) {
            trackState.audioMuted = audioMuted;
            applyCommand(shellState, trackState);
        }
        ImGui::TableNextColumn();
        bool audioSolo = trackState.audioSolo;
        if (ImGui::Checkbox("##audioSolo", &audioSolo)) {
            trackState.audioSolo = audioSolo;
            applyCommand(shellState, trackState);
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void drawInspectorPanel(ShellState* shellState, const jcut::EditorDocumentCore& snapshot)
{
    const bool focusOutput = std::exchange(shellState->focusInspectorOutputRequested, false);
    const bool focusProjects = std::exchange(shellState->focusInspectorProjectsRequested, false);
    const ShellLayout layout = computeShellLayout();
    const ImGuiCond layoutCondition = shellState->resetLayoutRequested
        ? ImGuiCond_Always
        : ImGuiCond_FirstUseEver;
    ImGui::SetNextWindowPos(layout.inspector.pos, layoutCondition);
    ImGui::SetNextWindowSize(layout.inspector.size, layoutCondition);
    if (focusOutput || focusProjects) {
        ImGui::SetNextWindowFocus();
    }
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse;
    ImGui::Begin("Inspector", nullptr, flags);
    const jcut::EditorClip* currentClip = selectedClip(snapshot);
    const jcut::EditorTrack* currentTrack = selectedTrack(snapshot);
    const std::int64_t currentClipLocalFrame = currentClip
        ? clipLocalPlayheadFrame(snapshot, *currentClip)
        : 0;
    const std::int64_t currentClipLastFrame = currentClip
        ? std::max<std::int64_t>(0, currentClip->durationFrames - 1)
        : 0;
    const std::int64_t fadeEndFrame =
        std::min(currentClipLocalFrame + 15, currentClipLastFrame);
    if (ImGui::BeginTabBar("InspectorTabs")) {
        if (ImGui::BeginTabItem("Grade")) {
            drawInspectorHeading("Grade", snapshot, currentClip);
            float saturation = currentClip ? static_cast<float>(currentClip->saturation) : 1.0f;
            float brightness = currentClip ? static_cast<float>(currentClip->brightness) : 0.0f;
            float contrast = currentClip ? static_cast<float>(currentClip->contrast) : 1.0f;
            bool gradePreview = currentClip ? currentClip->gradingPreviewEnabled : false;
            ImGui::BeginDisabled(!currentClip);
            bool gradingChanged = false;
            gradingChanged |= ImGui::SliderFloat("Saturation", &saturation, 0.0f, 2.0f, "%.2f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            gradingChanged |= ImGui::SliderFloat("Brightness", &brightness, -1.0f, 1.0f, "%.2f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            gradingChanged |= ImGui::SliderFloat("Contrast", &contrast, 0.0f, 2.0f, "%.2f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            gradingChanged |= ImGui::Checkbox("Preview", &gradePreview);
            if (gradingChanged && currentClip) {
                applyCommand(shellState, jcut::SetClipGradingCommand{
                    currentClip->id, brightness, contrast, saturation, gradePreview});
            }
            const PreviewHistogram histogram = currentPreviewHistogram(shellState);
            if (ImGui::BeginTabBar("GradeChannels")) {
                for (const char* channel : {"Red", "Green", "Blue", "Brightness"}) {
                    if (ImGui::BeginTabItem(channel)) {
                        const std::array<float, PreviewHistogram::kBinCount>* values = &histogram.luma;
                        if (std::strcmp(channel, "Red") == 0) {
                            values = &histogram.red;
                        } else if (std::strcmp(channel, "Green") == 0) {
                            values = &histogram.green;
                        } else if (std::strcmp(channel, "Blue") == 0) {
                            values = &histogram.blue;
                        }
                        if (histogram.valid) {
                            ImGui::PlotHistogram("Histogram",
                                                 values->data(),
                                                 static_cast<int>(values->size()),
                                                 0,
                                                 nullptr,
                                                 0.0f,
                                                 1.0f,
                                                 ImVec2(-1.0f, 72.0f));
                        } else {
                            ImGui::TextDisabled("Histogram requires a CPU preview frame");
                            ImGui::Dummy(ImVec2(-1.0f, 54.0f));
                        }
                        ImGui::EndTabItem();
                    }
                }
                ImGui::EndTabBar();
            }
            const jcut::EditorGradingKeyframe gradingInitial = currentClip
                ? jcut::evaluateEditorClipGradingAtLocalFrame(
                      *currentClip, currentClipLocalFrame)
                : jcut::EditorGradingKeyframe{};
            drawKeyframeDraftEditor<
                jcut::EditorGradingKeyframe,
                jcut::UpsertGradingKeyframeCommand>(
                    shellState,
                    currentClip,
                    jcut::EditorKeyframeChannel::Grading,
                    currentClipLastFrame,
                    "GradeKeyframeEditor",
                    gradingInitial,
                    [](jcut::EditorGradingKeyframe* draft) {
                        ImGui::InputDouble(
                            "Brightness", &draft->brightness, 0.01, 0.1, "%.3f");
                        ImGui::InputDouble(
                            "Contrast", &draft->contrast, 0.01, 0.1, "%.3f");
                        ImGui::InputDouble(
                            "Saturation", &draft->saturation, 0.01, 0.1, "%.3f");
                        ImGui::InputDouble(
                            "Grade Opacity", &draft->opacity, 0.01, 0.1, "%.3f");
                        if (ImGui::TreeNode("Lift / Gamma / Gain")) {
                            const auto editToneRgb = [](
                                const char* label,
                                double* red,
                                double* green,
                                double* blue) -> bool {
                                std::array<double, 3> values{
                                    *red, *green, *blue};
                                constexpr double minimum = -2.0;
                                constexpr double maximum = 2.0;
                                if (ImGui::SliderScalarN(
                                        label,
                                        ImGuiDataType_Double,
                                        values.data(),
                                        static_cast<int>(values.size()),
                                        &minimum,
                                        &maximum,
                                        "%.3f")) {
                                    *red = values[0];
                                    *green = values[1];
                                    *blue = values[2];
                                    return true;
                                }
                                return false;
                            };
                            bool toneValuesChanged = false;
                            toneValuesChanged |= editToneRgb(
                                "Lift RGB",
                                &draft->shadowsR,
                                &draft->shadowsG,
                                &draft->shadowsB);
                            toneValuesChanged |= editToneRgb(
                                "Gamma RGB",
                                &draft->midtonesR,
                                &draft->midtonesG,
                                &draft->midtonesB);
                            toneValuesChanged |= editToneRgb(
                                "Gain RGB",
                                &draft->highlightsR,
                                &draft->highlightsG,
                                &draft->highlightsB);
                            if (toneValuesChanged && draft->curveThreePointLock) {
                                jcut::synchronizeEditorThreePointGradingCurves(draft);
                            }
                            ImGui::TreePop();
                        }
                        if (ImGui::Button("Normalize curves")) {
                            jcut::normalizeEditorGradingCurves(*draft);
                        }
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip(
                                "Fold Luma into RGB and simplify each channel to at most 12 points");
                        }
                        if (ImGui::Checkbox(
                                "Three-point lock", &draft->curveThreePointLock) &&
                            draft->curveThreePointLock) {
                            jcut::synchronizeEditorThreePointGradingCurves(draft);
                        }
                        ImGui::Checkbox(
                            "Curve smoothing", &draft->curveSmoothingEnabled);
                        if (ImGui::TreeNode("Curves")) {
                            drawGradingCurvePointEditor(
                                "Red", &draft->curvePointsR, draft->curveThreePointLock);
                            drawGradingCurvePointEditor(
                                "Green", &draft->curvePointsG, draft->curveThreePointLock);
                            drawGradingCurvePointEditor(
                                "Blue", &draft->curvePointsB, draft->curveThreePointLock);
                            drawGradingCurvePointEditor(
                                "Luma", &draft->curvePointsLuma, false);
                            ImGui::TreePop();
                        }
                    });
            ImGui::BeginDisabled();
            ImGui::Button("Auto Oppose (Qt workflow)");
            ImGui::EndDisabled();
            if (ImGui::BeginTable(
                    "GradeKeys",
                    7,
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                        ImGuiTableFlags_ScrollX)) {
                ImGui::TableSetupColumn("Frame");
                ImGui::TableSetupColumn("Bright");
                ImGui::TableSetupColumn("Contrast");
                ImGui::TableSetupColumn("Sat");
                ImGui::TableSetupColumn("Opacity");
                ImGui::TableSetupColumn("Interp");
                ImGui::TableSetupColumn("Actions");
                ImGui::TableHeadersRow();
                if (currentClip) {
                    for (const jcut::EditorGradingKeyframe& keyframe : currentClip->gradingKeyframes) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        drawFrameSeekCell(
                            shellState,
                            keyframe.frame,
                            static_cast<std::int64_t>(currentClip->startFrame) + keyframe.frame,
                            "grading-frame-" + std::to_string(keyframe.frame));
                        ImGui::TableNextColumn();
                        ImGui::Text("%.2f", keyframe.brightness);
                        ImGui::TableNextColumn();
                        ImGui::Text("%.2f", keyframe.contrast);
                        ImGui::TableNextColumn();
                        ImGui::Text("%.2f", keyframe.saturation);
                        ImGui::TableNextColumn();
                        ImGui::Text("%.2f", keyframe.opacity);
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(keyframe.linearInterpolation ? "Linear" : "Hold");
                        ImGui::TableNextColumn();
                        const std::string keyId = "grading-" +
                            std::to_string(keyframe.frame);
                        ImGui::PushID(keyId.c_str());
                        if (ImGui::SmallButton("Load/Edit")) {
                            loadKeyframeDraft(
                                shellState,
                                currentClip->id,
                                jcut::EditorKeyframeChannel::Grading,
                                keyframe);
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton(
                                keyframe.linearInterpolation
                                    ? "Set Hold"
                                    : "Set Linear")) {
                            jcut::EditorGradingKeyframe updated = keyframe;
                            updated.linearInterpolation =
                                !updated.linearInterpolation;
                            applyCommand(shellState,
                                         jcut::UpsertGradingKeyframeCommand{
                                             currentClip->id,
                                             std::move(updated)});
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Remove")) {
                            removeInspectorKeyframe(
                                shellState,
                                currentClip->id,
                                jcut::EditorKeyframeChannel::Grading,
                                keyframe.frame);
                        }
                        ImGui::PopID();
                    }
                }
                ImGui::EndTable();
            }
            ImGui::EndDisabled();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Opacity")) {
            drawInspectorHeading("Opacity", snapshot, currentClip);
            float opacity = currentClip ? static_cast<float>(currentClip->opacity) : 1.0f;
            ImGui::BeginDisabled(!currentClip);
            const bool opacityChanged = ImGui::SliderFloat("Opacity", &opacity, 0.0f, 1.0f, "%.2f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            if (opacityChanged && currentClip) {
                applyCommand(shellState, jcut::SetClipOpacityCommand{currentClip->id, opacity});
            }
            const jcut::EditorOpacityKeyframe opacityInitial{
                currentClipLocalFrame, opacity, true};
            drawKeyframeDraftEditor<
                jcut::EditorOpacityKeyframe,
                jcut::UpsertOpacityKeyframeCommand>(
                    shellState,
                    currentClip,
                    jcut::EditorKeyframeChannel::Opacity,
                    currentClipLastFrame,
                    "OpacityKeyframeEditor",
                    opacityInitial,
                    [](jcut::EditorOpacityKeyframe* draft) {
                        ImGui::InputDouble(
                            "Key Opacity", &draft->opacity, 0.01, 0.1, "%.3f");
                    });
            if (ImGui::Button("Fade In From Playhead") && currentClip) {
                beginRuntimeHistoryTransaction(shellState);
                applyCommand(shellState, jcut::UpsertOpacityKeyframeCommand{
                    currentClip->id, {currentClipLocalFrame, 0.0, true}});
                applyCommand(shellState, jcut::UpsertOpacityKeyframeCommand{
                    currentClip->id, {fadeEndFrame, opacity, true}});
                endRuntimeHistoryTransaction(shellState);
            }
            ImGui::SameLine();
            if (ImGui::Button("Fade Out From Playhead") && currentClip) {
                beginRuntimeHistoryTransaction(shellState);
                applyCommand(shellState, jcut::UpsertOpacityKeyframeCommand{
                    currentClip->id, {currentClipLocalFrame, opacity, true}});
                applyCommand(shellState, jcut::UpsertOpacityKeyframeCommand{
                    currentClip->id, {fadeEndFrame, 0.0, true}});
                endRuntimeHistoryTransaction(shellState);
            }
            if (ImGui::BeginTable(
                    "OpacityKeys",
                    4,
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                        ImGuiTableFlags_ScrollX)) {
                ImGui::TableSetupColumn("Frame");
                ImGui::TableSetupColumn("Opacity");
                ImGui::TableSetupColumn("Interp");
                ImGui::TableSetupColumn("Actions");
                ImGui::TableHeadersRow();
                if (currentClip) {
                    for (const jcut::EditorOpacityKeyframe& keyframe : currentClip->opacityKeyframes) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        drawFrameSeekCell(
                            shellState,
                            keyframe.frame,
                            static_cast<std::int64_t>(currentClip->startFrame) + keyframe.frame,
                            "opacity-frame-" + std::to_string(keyframe.frame));
                        ImGui::TableNextColumn();
                        ImGui::Text("%.2f", keyframe.opacity);
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(keyframe.linearInterpolation ? "Linear" : "Hold");
                        ImGui::TableNextColumn();
                        const std::string keyId = "opacity-" +
                            std::to_string(keyframe.frame);
                        ImGui::PushID(keyId.c_str());
                        if (ImGui::SmallButton("Load/Edit")) {
                            loadKeyframeDraft(
                                shellState,
                                currentClip->id,
                                jcut::EditorKeyframeChannel::Opacity,
                                keyframe);
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton(
                                keyframe.linearInterpolation
                                    ? "Set Hold"
                                    : "Set Linear")) {
                            jcut::EditorOpacityKeyframe updated = keyframe;
                            updated.linearInterpolation =
                                !updated.linearInterpolation;
                            applyCommand(shellState,
                                         jcut::UpsertOpacityKeyframeCommand{
                                             currentClip->id,
                                             std::move(updated)});
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Remove")) {
                            removeInspectorKeyframe(
                                shellState,
                                currentClip->id,
                                jcut::EditorKeyframeChannel::Opacity,
                                keyframe.frame);
                        }
                        ImGui::PopID();
                    }
                }
                ImGui::EndTable();
            }
            ImGui::EndDisabled();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Effects")) {
            drawInspectorHeading("Effects", snapshot, currentClip);
            std::string presetId = currentClip ? currentClip->effectPreset : "none";
            const std::string presetLabel = effectPresetDisplayName(presetId);
            bool alternate = currentClip ? currentClip->effectAlternateDirection : true;
            const std::string effectMediaKind = currentClip
                ? lowerAscii(currentClip->mediaKind)
                : std::string();
            const bool imagePresetCapable = currentClip &&
                (effectMediaKind == "image" || effectMediaKind == "video");
            ImGui::BeginDisabled(!imagePresetCapable);
            bool effectChanged = false;
            if (ImGui::BeginCombo("Preset", presetLabel.c_str())) {
                for (const std::string_view optionId : jcut::kEditorEffectPresetIds) {
                    const bool selected = presetId == optionId;
                    const std::string optionLabel = effectPresetDisplayName(optionId);
                    if (ImGui::Selectable(optionLabel.c_str(), selected)) {
                        presetId = optionId;
                        effectChanged = true;
                    }
                    if (selected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }

            const bool edge = presetId == "sobel_edges";
            const bool neon = presetId == "neon_glow";
            const bool speakerMask = effectPresetIsSpeakerMask(presetId);
            const bool progressiveEdge = presetId == "progressive_edge_stretch";
            const bool commonParameters =
                effectPresetUsesCommonNeutralParameters(presetId);
            const int contextualMaxRows = edge || neon
                ? 4
                : (speakerMask
                       ? 8
                       : jcut::editorEffectMaxRowsForPreset(presetId));
            int rows = std::clamp(
                currentClip ? currentClip->effectRows : 32,
                jcut::kEditorEffectMinRows,
                contextualMaxRows);
            float speed = std::clamp(
                currentClip ? static_cast<float>(currentClip->effectSpeed) : 1.0f,
                static_cast<float>(jcut::kEditorEffectMinSpeed),
                static_cast<float>(jcut::kEditorEffectMaxSpeed));
            const float contextualMaxScale = speakerMask
                ? 1.0f
                : static_cast<float>(jcut::kEditorEffectMaxScale);
            float scale = std::clamp(
                currentClip ? static_cast<float>(currentClip->effectScale) : 1.0f,
                static_cast<float>(jcut::kEditorEffectMinScale),
                contextualMaxScale);
            bool speechSync = currentClip ? currentClip->effectSkipAwareTiming : true;
            int differenceReference = currentClip ? currentClip->differenceReferenceFrames : 1;
            float differenceThreshold = currentClip
                ? static_cast<float>(currentClip->differenceThreshold) : 0.10f;
            float differenceSoftness = currentClip
                ? static_cast<float>(currentClip->differenceSoftness) : 0.05f;
            int echoCount = currentClip ? currentClip->temporalEchoCount : 4;
            int echoSpacing = currentClip ? currentClip->temporalEchoSpacingFrames : 2;
            float echoDecay = currentClip
                ? static_cast<float>(currentClip->temporalEchoDecay) : 0.65f;
            std::string tilingPattern = currentClip ? currentClip->tilingPattern : "grid";
            float tilingSpacing = currentClip
                ? static_cast<float>(currentClip->tilingSpacing) : 1.0f;
            bool tilingWrap = currentClip ? currentClip->tilingWrap : true;

            if (commonParameters) {
                const char* rowsLabel = edge
                    ? "Sample Radius"
                    : (neon
                           ? "Glow Radius"
                           : (speakerMask
                                  ? "Dilation Radius"
                                  : (progressiveEdge ? "Edge Width" : "Copies")));
                effectChanged |= ImGui::SliderInt(
                    rowsLabel,
                    &rows,
                    jcut::kEditorEffectMinRows,
                    contextualMaxRows);
                beginRuntimeHistoryTransactionForLastItem(shellState);

                if (!edge) {
                    const char* speedLabel = neon
                        ? "Hue Speed"
                        : (speakerMask ? "Color Cycle Speed" : "Speed");
                    ImGui::BeginDisabled(progressiveEdge);
                    effectChanged |= ImGui::SliderFloat(
                        speedLabel,
                        &speed,
                        static_cast<float>(jcut::kEditorEffectMinSpeed),
                        static_cast<float>(jcut::kEditorEffectMaxSpeed),
                        "%.2f");
                    beginRuntimeHistoryTransactionForLastItem(shellState);
                    ImGui::EndDisabled();
                }

                const char* scaleLabel = edge
                    ? "Edge Strength"
                    : (neon
                           ? "Glow Intensity"
                           : (speakerMask
                                  ? "Opacity"
                                  : (progressiveEdge ? "Curve Power" : "Scale")));
                effectChanged |= ImGui::SliderFloat(
                    scaleLabel,
                    &scale,
                    static_cast<float>(jcut::kEditorEffectMinScale),
                    contextualMaxScale,
                    "%.2f");
                beginRuntimeHistoryTransactionForLastItem(shellState);

                if (!edge && !neon && !speakerMask) {
                    ImGui::BeginDisabled(progressiveEdge);
                    effectChanged |= ImGui::Checkbox(
                        "Alternate Direction", &alternate);
                    ImGui::EndDisabled();
                }
            }
            if (commonParameters && !edge && !neon && !speakerMask &&
                !progressiveEdge) {
                effectChanged |= ImGui::Checkbox(
                    "Synchronize motion with Speech Filter", &speechSync);
            }
            if (presetId == "difference_matte") {
                effectChanged |= ImGui::SliderInt(
                    "Difference Reference Frames", &differenceReference, 1, 300);
                beginRuntimeHistoryTransactionForLastItem(shellState);
                effectChanged |= ImGui::SliderFloat(
                    "Difference Threshold", &differenceThreshold, 0.0f, 1.0f, "%.2f");
                beginRuntimeHistoryTransactionForLastItem(shellState);
                effectChanged |= ImGui::SliderFloat(
                    "Difference Softness", &differenceSoftness, 0.0f, 1.0f, "%.2f");
                beginRuntimeHistoryTransactionForLastItem(shellState);
            } else if (presetId == "temporal_echo") {
                effectChanged |= ImGui::SliderInt("Echo Count", &echoCount, 1, 12);
                beginRuntimeHistoryTransactionForLastItem(shellState);
                effectChanged |= ImGui::SliderInt("Echo Spacing", &echoSpacing, 1, 120);
                beginRuntimeHistoryTransactionForLastItem(shellState);
                effectChanged |= ImGui::SliderFloat(
                    "Echo Decay", &echoDecay, 0.0f, 1.0f, "%.2f");
                beginRuntimeHistoryTransactionForLastItem(shellState);
            }
            if (effectPresetUsesTilingParameters(presetId)) {
                static constexpr std::array<std::pair<std::string_view, const char*>, 6>
                    kTilingPatterns = {{
                        {"grid", "Grid"},
                        {"encircle", "Encircle"},
                        {"spiral_xy", "Spiral XY"},
                        {"spiral_xz", "Spiral XZ"},
                        {"spiral_yz", "Spiral YZ"},
                        {"diamond", "Diamond"},
                    }};
                const char* patternLabel = "Grid";
                for (const auto& [id, label] : kTilingPatterns) {
                    if (tilingPattern == id) {
                        patternLabel = label;
                    }
                }
                if (ImGui::BeginCombo("Tiling Pattern", patternLabel)) {
                    for (const auto& [id, label] : kTilingPatterns) {
                        const bool selected = tilingPattern == id;
                        if (ImGui::Selectable(label, selected)) {
                            tilingPattern = id;
                            effectChanged = true;
                        }
                        if (selected) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }
                effectChanged |= ImGui::SliderFloat(
                    "Tiling Spacing", &tilingSpacing, 0.1f, 8.0f, "%.2f");
                beginRuntimeHistoryTransactionForLastItem(shellState);
                effectChanged |= ImGui::Checkbox("Wrap Across Bounds", &tilingWrap);
            }
            if (effectChanged && currentClip) {
                jcut::SetClipMaskEffectCommand command;
                command.clipId = currentClip->id;
                command.maskEnabled = currentClip->maskEnabled;
                command.feather = currentClip->maskFeather;
                command.featherGamma = currentClip->maskFeatherGamma;
                command.featherFalloff = currentClip->maskFeatherFalloff;
                command.foregroundLayerEnabled = currentClip->maskForegroundLayerEnabled;
                command.repeatEnabled = currentClip->maskRepeatEnabled;
                command.repeatDeltaX = currentClip->maskRepeatDeltaX;
                command.repeatDeltaY = currentClip->maskRepeatDeltaY;
                command.effectPreset = presetId;
                command.effectRows = rows;
                command.effectSpeed = speed;
                command.effectScale = scale;
                command.alternateDirection = alternate;
                command.skipAwareTiming = speechSync;
                command.differenceReferenceFrames = differenceReference;
                command.differenceThreshold = differenceThreshold;
                command.differenceSoftness = differenceSoftness;
                command.temporalEchoCount = echoCount;
                command.temporalEchoSpacingFrames = echoSpacing;
                command.temporalEchoDecay = echoDecay;
                command.tilingPattern = tilingPattern;
                command.tilingSpacing = tilingSpacing;
                command.tilingWrap = tilingWrap;
                applyCommand(shellState, std::move(command));
            }
            ImGui::EndDisabled();
            if (currentClip && !imagePresetCapable) {
                ImGui::TextWrapped(
                    "Effect presets require an image or video clip.");
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Masks")) {
            drawInspectorHeading("Masks", snapshot, currentClip);
            const jcut::EditorClip* maskSourceClip = nullptr;
            const jcut::EditorClip* maskEditClip = nullptr;
            if (currentClip && currentClip->clipRole == "mask_matte") {
                maskEditClip = currentClip;
                const auto source = std::find_if(
                    snapshot.clips.begin(), snapshot.clips.end(),
                    [&](const jcut::EditorClip& candidate) {
                        return candidate.persistentId == currentClip->linkedSourceClipId &&
                            candidate.clipRole != "mask_matte";
                    });
                if (source != snapshot.clips.end()) maskSourceClip = &*source;
            } else if (currentClip && currentClip->clipRole != "mask_matte" &&
                       currentClip->mediaKind == "video") {
                maskSourceClip = currentClip;
            }
            if (maskSourceClip &&
                shellState->maskSidecarContextClipId != maskSourceClip->id) {
                shellState->maskSidecarContextClipId = maskSourceClip->id;
                shellState->maskSidecarDirectoryDraft =
                    maskEditClip ? maskEditClip->maskFramesDir : maskSourceClip->maskFramesDir;
                const fs::path sourcePath = resolvedClipMediaPathForProbe(
                    *shellState, *maskSourceClip);
                fs::path preferred(shellState->maskSidecarDirectoryDraft);
                if (preferred.is_relative() && !shellState->projectRootPath.empty()) {
                    preferred = fs::path(shellState->projectRootPath) / preferred;
                }
                shellState->maskSidecars = jcut::masks::discoverMaskSidecarsCore(
                    sourcePath, preferred);
            }
            if (!maskSourceClip) {
                shellState->maskSidecarContextClipId = -1;
                shellState->maskSidecars.clear();
            }
            ImGui::SeparatorText("Generated Mask Sidecars");
            if (maskSourceClip) {
                ImGui::TextWrapped(
                    "Source: %s. Each ready sidecar materializes as its own generated child lane.",
                    maskSourceClip->label.c_str());
                if (ImGui::Button("Refresh Sidecars")) {
                    shellState->maskSidecarContextClipId = -1;
                }
                ImGui::SetNextItemWidth(
                    std::max(120.0f, ImGui::GetContentRegionAvail().x - 110.0f));
                inputTextForString<512>(
                    "Sidecar Directory", &shellState->maskSidecarDirectoryDraft);
                ImGui::SameLine();
                if (ImGui::Button("Inspect / Use")) {
                    fs::path directory(shellState->maskSidecarDirectoryDraft);
                    if (directory.is_relative() && !shellState->projectRootPath.empty()) {
                        directory = fs::path(shellState->projectRootPath) / directory;
                    }
                    const auto sidecar = jcut::masks::inspectMaskSidecarCore(
                        directory, resolvedClipMediaPathForProbe(*shellState, *maskSourceClip));
                    if (!sidecar.valid()) {
                        shellState->statusMessage = "mask sidecar contains no frame_*.png files";
                    } else if (!sidecar.ready) {
                        shellState->statusMessage = sidecar.readinessIssue.empty()
                            ? "mask sidecar is not render-ready" : sidecar.readinessIssue;
                    } else {
                        applyCommand(shellState, jcut::MaterializeMaskMatteCommand{
                            maskSourceClip->id,
                            sidecar.directory.string(), sidecar.id, sidecar.displayName});
                        shellState->maskSidecarContextClipId = -1;
                    }
                }
                if (ImGui::BeginTable(
                        "MaskSidecars", 4,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                    ImGui::TableSetupColumn("Sidecar");
                    ImGui::TableSetupColumn("Frames");
                    ImGui::TableSetupColumn("Status");
                    ImGui::TableSetupColumn("Action");
                    ImGui::TableHeadersRow();
                    for (const auto& sidecar : shellState->maskSidecars) {
                        ImGui::PushID(sidecar.id.c_str());
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(sidecar.displayName.c_str());
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("%s", sidecar.directory.string().c_str());
                        }
                        ImGui::TableNextColumn();
                        ImGui::Text("%lld", static_cast<long long>(sidecar.frameCount));
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(sidecar.ready
                            ? "Ready"
                            : (sidecar.readinessIssue.empty()
                                   ? "Unavailable" : sidecar.readinessIssue.c_str()));
                        ImGui::TableNextColumn();
                        ImGui::BeginDisabled(!sidecar.ready);
                        if (ImGui::SmallButton("Use")) {
                            applyCommand(shellState, jcut::MaterializeMaskMatteCommand{
                                maskSourceClip->id,
                                sidecar.directory.string(), sidecar.id,
                                sidecar.displayName});
                            shellState->maskSidecarContextClipId = -1;
                        }
                        ImGui::EndDisabled();
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
            } else {
                ImGui::TextWrapped("Select a video source or generated Mask Matte child.");
            }
            ImGui::SeparatorText("Selected Matte Treatment");
            if (maskSourceClip && !maskEditClip) {
                ImGui::TextDisabled(
                    "Choose a ready sidecar above; treatment controls belong to its child lane.");
            }
            if (maskEditClip) {
                int zLevel = maskEditClip->zLevel == std::numeric_limits<int>::min()
                    ? -std::max(0, maskEditClip->trackId - 1) * 100
                    : maskEditClip->zLevel;
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::InputInt("Z Level", &zLevel)) {
                    applyCommand(shellState, jcut::SetClipZLevelCommand{
                        maskEditClip->id, zLevel, false});
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Automatic Z")) {
                    applyCommand(shellState, jcut::SetClipZLevelCommand{
                        maskEditClip->id, 0, true});
                }
            }
            bool enabled = currentClip ? currentClip->maskEnabled : false;
            float radius = currentClip ? static_cast<float>(currentClip->maskFeather) : 0.0f;
            float gamma = currentClip ? static_cast<float>(currentClip->maskFeatherGamma) : 1.0f;
            int falloff = currentClip ? currentClip->maskFeatherFalloff : 0;
            bool foreground = currentClip ? currentClip->maskForegroundLayerEnabled : false;
            bool repeat = currentClip ? currentClip->maskRepeatEnabled : false;
            float repeatX = currentClip ? static_cast<float>(currentClip->maskRepeatDeltaX) : 160.0f;
            float repeatY = currentClip ? static_cast<float>(currentClip->maskRepeatDeltaY) : 0.0f;
            float dilate = currentClip ? static_cast<float>(currentClip->maskDilate) : 0.0f;
            float erode = currentClip ? static_cast<float>(currentClip->maskErode) : 0.0f;
            float blur = currentClip ? static_cast<float>(currentClip->maskBlur) : 0.0f;
            bool invert = currentClip ? currentClip->maskInvert : false;
            bool showOnly = currentClip ? currentClip->maskShowOnly : false;
            float opacity = currentClip ? static_cast<float>(currentClip->maskOpacity) : 1.0f;
            bool gradeEnabled = currentClip ? currentClip->maskGradeEnabled : false;
            float gradeBrightness = currentClip
                ? static_cast<float>(currentClip->maskGradeBrightness) : 0.0f;
            float gradeContrast = currentClip
                ? static_cast<float>(currentClip->maskGradeContrast) : 1.0f;
            float gradeSaturation = currentClip
                ? static_cast<float>(currentClip->maskGradeSaturation) : 1.0f;
            std::vector<jcut::EditorPoint> gradeCurveR = currentClip
                ? currentClip->maskGradeCurvePointsR
                : std::vector<jcut::EditorPoint>{{0.0, 0.0}, {1.0, 1.0}};
            std::vector<jcut::EditorPoint> gradeCurveG = currentClip
                ? currentClip->maskGradeCurvePointsG
                : std::vector<jcut::EditorPoint>{{0.0, 0.0}, {1.0, 1.0}};
            std::vector<jcut::EditorPoint> gradeCurveB = currentClip
                ? currentClip->maskGradeCurvePointsB
                : std::vector<jcut::EditorPoint>{{0.0, 0.0}, {1.0, 1.0}};
            std::vector<jcut::EditorPoint> gradeCurveLuma = currentClip
                ? currentClip->maskGradeCurvePointsLuma
                : std::vector<jcut::EditorPoint>{{0.0, 0.0}, {1.0, 1.0}};
            bool gradeCurveSmoothing = currentClip
                ? currentClip->maskGradeCurveSmoothingEnabled : false;
            bool shadowEnabled = currentClip ? currentClip->maskDropShadowEnabled : false;
            float shadowRadius = currentClip
                ? static_cast<float>(currentClip->maskDropShadowRadius) : 12.0f;
            float shadowOffsetX = currentClip
                ? static_cast<float>(currentClip->maskDropShadowOffsetX) : 0.0f;
            float shadowOffsetY = currentClip
                ? static_cast<float>(currentClip->maskDropShadowOffsetY) : 4.0f;
            float shadowOpacity = currentClip
                ? static_cast<float>(currentClip->maskDropShadowOpacity) : 0.45f;
            ImGui::BeginDisabled(!maskEditClip);
            bool maskChanged = false;
            maskChanged |= ImGui::Checkbox("Enabled", &enabled);
            maskChanged |= ImGui::SliderFloat("Feather Radius", &radius, 0.0f, 256.0f, "%.1f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            maskChanged |= ImGui::SliderFloat("Feather Gamma", &gamma, 0.1f, 8.0f, "%.2f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            maskChanged |= ImGui::SliderInt("Falloff", &falloff, 0, 5);
            beginRuntimeHistoryTransactionForLastItem(shellState);
            maskChanged |= ImGui::SliderFloat("Dilate", &dilate, 0.0f, 200.0f, "%.1f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            maskChanged |= ImGui::SliderFloat("Erode", &erode, 0.0f, 200.0f, "%.1f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            maskChanged |= ImGui::SliderFloat("Mask Blur", &blur, 0.0f, 200.0f, "%.1f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            maskChanged |= ImGui::Checkbox("Invert", &invert);
            ImGui::SameLine();
            maskChanged |= ImGui::Checkbox("Show Mask Only", &showOnly);
            maskChanged |= ImGui::SliderFloat("Mask Opacity", &opacity, 0.0f, 1.0f, "%.2f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            ImGui::SeparatorText("Masked Grade");
            maskChanged |= ImGui::Checkbox("Enable Mask Grade", &gradeEnabled);
            maskChanged |= ImGui::SliderFloat(
                "Mask Brightness", &gradeBrightness, -1.0f, 1.0f, "%.2f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            maskChanged |= ImGui::SliderFloat(
                "Mask Contrast", &gradeContrast, 0.0f, 4.0f, "%.2f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            maskChanged |= ImGui::SliderFloat(
                "Mask Saturation", &gradeSaturation, 0.0f, 4.0f, "%.2f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            maskChanged |= ImGui::Checkbox(
                "Mask Curve Smoothing", &gradeCurveSmoothing);
            bool maskCurveChanged = false;
            maskCurveChanged |= drawGradingCurvePointEditor(
                "Mask Red Curve", &gradeCurveR, false);
            maskCurveChanged |= drawGradingCurvePointEditor(
                "Mask Green Curve", &gradeCurveG, false);
            maskCurveChanged |= drawGradingCurvePointEditor(
                "Mask Blue Curve", &gradeCurveB, false);
            maskCurveChanged |= drawGradingCurvePointEditor(
                "Mask Luma Curve", &gradeCurveLuma, false);
            if (maskCurveChanged) {
                beginRuntimeHistoryTransaction(shellState);
                maskChanged = true;
            }
            ImGui::SeparatorText("Mask Shadow");
            maskChanged |= ImGui::Checkbox("Enable Mask Shadow", &shadowEnabled);
            maskChanged |= ImGui::SliderFloat(
                "Shadow Radius", &shadowRadius, 0.0f, 200.0f, "%.1f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            maskChanged |= ImGui::DragFloat("Shadow Offset X", &shadowOffsetX, 1.0f, -500.0f, 500.0f);
            beginRuntimeHistoryTransactionForLastItem(shellState);
            maskChanged |= ImGui::DragFloat("Shadow Offset Y", &shadowOffsetY, 1.0f, -500.0f, 500.0f);
            beginRuntimeHistoryTransactionForLastItem(shellState);
            maskChanged |= ImGui::SliderFloat(
                "Shadow Opacity", &shadowOpacity, 0.0f, 1.0f, "%.2f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            ImGui::SeparatorText("Mask Layers");
            maskChanged |= ImGui::Checkbox("Foreground Layer", &foreground);
            maskChanged |= ImGui::Checkbox("Repeat", &repeat);
            maskChanged |= ImGui::DragFloat("Repeat X", &repeatX, 1.0f);
            beginRuntimeHistoryTransactionForLastItem(shellState);
            maskChanged |= ImGui::DragFloat("Repeat Y", &repeatY, 1.0f);
            beginRuntimeHistoryTransactionForLastItem(shellState);
            if (maskChanged && maskEditClip) {
                jcut::SetClipMaskCommand command;
                command.clipId = maskEditClip->id;
                command.maskEnabled = enabled;
                command.feather = radius;
                command.featherGamma = gamma;
                command.featherFalloff = falloff;
                command.foregroundLayerEnabled = foreground;
                command.repeatEnabled = repeat;
                command.repeatDeltaX = repeatX;
                command.repeatDeltaY = repeatY;
                command.dilate = dilate;
                command.erode = erode;
                command.blur = blur;
                command.invert = invert;
                command.showOnly = showOnly;
                command.opacity = opacity;
                command.gradeEnabled = gradeEnabled;
                command.gradeBrightness = gradeBrightness;
                command.gradeContrast = gradeContrast;
                command.gradeSaturation = gradeSaturation;
                command.gradeCurvePointsR = std::move(gradeCurveR);
                command.gradeCurvePointsG = std::move(gradeCurveG);
                command.gradeCurvePointsB = std::move(gradeCurveB);
                command.gradeCurvePointsLuma = std::move(gradeCurveLuma);
                command.gradeCurveSmoothingEnabled = gradeCurveSmoothing;
                command.dropShadowEnabled = shadowEnabled;
                command.dropShadowRadius = shadowRadius;
                command.dropShadowOffsetX = shadowOffsetX;
                command.dropShadowOffsetY = shadowOffsetY;
                command.dropShadowOpacity = shadowOpacity;
                applyCommand(shellState, std::move(command));
            }
            ImGui::EndDisabled();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Corrections")) {
            drawInspectorHeading("Corrections", snapshot, currentClip);
            const bool supportsCorrections = currentClip && currentClip->mediaKind != "audio";
            if (!supportsCorrections || shellState->correctionClipId != currentClip->id) {
                shellState->correctionDrawMode = false;
                shellState->correctionClipId = currentClip ? currentClip->id : -1;
                shellState->selectedCorrectionPolygon = -1;
                shellState->correctionDraftPoints.clear();
            }
            bool enabled = snapshot.exportRequest.correctionsEnabled;
            if (ImGui::Checkbox("Enable Corrections", &enabled)) {
                applyCommand(shellState, jcut::SetCorrectionsEnabledCommand{enabled});
            }
            int deletePolygon = -1;
            if (ImGui::BeginTable("PolygonRanges", 5,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("On");
                ImGui::TableSetupColumn("Start");
                ImGui::TableSetupColumn("End");
                ImGui::TableSetupColumn("Points");
                ImGui::TableSetupColumn("Action");
                ImGui::TableHeadersRow();
                if (currentClip) {
                    for (std::size_t polygonIndex = 0;
                         polygonIndex < currentClip->correctionPolygons.size();
                         ++polygonIndex) {
                        const jcut::EditorCorrectionPolygon& polygon =
                            currentClip->correctionPolygons[polygonIndex];
                        ImGui::PushID(static_cast<int>(polygonIndex));
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        bool polygonEnabled = polygon.enabled;
                        if (ImGui::Checkbox("##enabled", &polygonEnabled)) {
                            auto polygons = currentClip->correctionPolygons;
                            polygons[polygonIndex].enabled = polygonEnabled;
                            applyCommand(shellState, jcut::SetClipCorrectionPolygonsCommand{
                                currentClip->id, std::move(polygons)});
                        }
                        ImGui::TableNextColumn();
                        std::int64_t startFrame = polygon.startFrame;
                        ImGui::SetNextItemWidth(-FLT_MIN);
                        const bool startChanged =
                            ImGui::InputScalar("##start", ImGuiDataType_S64, &startFrame);
                        beginRuntimeHistoryTransactionForLastItem(shellState);
                        if (startChanged) {
                            auto polygons = currentClip->correctionPolygons;
                            polygons[polygonIndex].startFrame = std::max<std::int64_t>(0, startFrame);
                            applyCommand(shellState, jcut::SetClipCorrectionPolygonsCommand{
                                currentClip->id, std::move(polygons)});
                        }
                        ImGui::TableNextColumn();
                        std::int64_t endFrame = polygon.endFrame;
                        ImGui::SetNextItemWidth(-FLT_MIN);
                        const bool endChanged =
                            ImGui::InputScalar("##end", ImGuiDataType_S64, &endFrame);
                        beginRuntimeHistoryTransactionForLastItem(shellState);
                        if (endChanged) {
                            auto polygons = currentClip->correctionPolygons;
                            polygons[polygonIndex].endFrame = endFrame;
                            applyCommand(shellState, jcut::SetClipCorrectionPolygonsCommand{
                                currentClip->id, std::move(polygons)});
                        }
                        ImGui::TableNextColumn();
                        const std::string pointLabel = std::to_string(polygon.pointsNormalized.size());
                        if (ImGui::Selectable(pointLabel.c_str(),
                                              shellState->selectedCorrectionPolygon ==
                                                  static_cast<int>(polygonIndex))) {
                            shellState->selectedCorrectionPolygon = static_cast<int>(polygonIndex);
                        }
                        ImGui::TableNextColumn();
                        if (ImGui::SmallButton("Delete")) {
                            deletePolygon = static_cast<int>(polygonIndex);
                        }
                        ImGui::PopID();
                    }
                }
                ImGui::EndTable();
            }
            if (deletePolygon >= 0 && currentClip) {
                auto polygons = currentClip->correctionPolygons;
                polygons.erase(polygons.begin() + deletePolygon);
                applyCommand(shellState, jcut::SetClipCorrectionPolygonsCommand{
                    currentClip->id, std::move(polygons)});
                shellState->selectedCorrectionPolygon = -1;
            }
            ImGui::BeginDisabled(!supportsCorrections);
            if (ImGui::Button(shellState->correctionDrawMode ? "Drawing Polygon..." : "Draw Polygon")) {
                shellState->correctionDrawMode = !shellState->correctionDrawMode;
                shellState->correctionDraftPoints.clear();
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(shellState->correctionDraftPoints.size() < 3);
            if (ImGui::Button("Close Polygon") && currentClip) {
                auto polygons = currentClip->correctionPolygons;
                jcut::EditorCorrectionPolygon polygon;
                polygon.pointsNormalized = shellState->correctionDraftPoints;
                polygons.push_back(std::move(polygon));
                applyCommand(shellState, jcut::SetClipCorrectionPolygonsCommand{
                    currentClip->id, std::move(polygons)});
                shellState->selectedCorrectionPolygon =
                    static_cast<int>(currentClip->correctionPolygons.size());
                shellState->correctionDraftPoints.clear();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(shellState->correctionDraftPoints.empty());
            if (ImGui::Button("Cancel Draft")) {
                shellState->correctionDraftPoints.clear();
            }
            ImGui::EndDisabled();
            ImGui::EndDisabled();
            if (shellState->correctionDrawMode) {
                ImGui::TextDisabled("Click the Program monitor to add points (%zu drafted).",
                                    shellState->correctionDraftPoints.size());
            }
            if (currentClip && shellState->selectedCorrectionPolygon >= 0 &&
                shellState->selectedCorrectionPolygon <
                    static_cast<int>(currentClip->correctionPolygons.size())) {
                const int polygonIndex = shellState->selectedCorrectionPolygon;
                const auto& points = currentClip->correctionPolygons[polygonIndex].pointsNormalized;
                ImGui::SeparatorText("Selected Polygon Vertices");
                int deletePoint = -1;
                for (std::size_t pointIndex = 0; pointIndex < points.size(); ++pointIndex) {
                    ImGui::PushID(static_cast<int>(pointIndex));
                    float coordinates[2] = {
                        static_cast<float>(points[pointIndex].x),
                        static_cast<float>(points[pointIndex].y)};
                    ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x - 70.0f));
                    const bool vertexChanged = ImGui::DragFloat2(
                        "##vertex", coordinates, 0.0025f, 0.0f, 1.0f, "%.4f");
                    beginRuntimeHistoryTransactionForLastItem(shellState);
                    if (vertexChanged) {
                        auto polygons = currentClip->correctionPolygons;
                        polygons[polygonIndex].pointsNormalized[pointIndex] = {
                            coordinates[0], coordinates[1]};
                        applyCommand(shellState, jcut::SetClipCorrectionPolygonsCommand{
                            currentClip->id, std::move(polygons)});
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Remove")) {
                        deletePoint = static_cast<int>(pointIndex);
                    }
                    ImGui::PopID();
                }
                if (deletePoint >= 0) {
                    auto polygons = currentClip->correctionPolygons;
                    auto& editablePoints = polygons[polygonIndex].pointsNormalized;
                    editablePoints.erase(editablePoints.begin() + deletePoint);
                    applyCommand(shellState, jcut::SetClipCorrectionPolygonsCommand{
                        currentClip->id, std::move(polygons)});
                }
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(!currentClip || currentClip->correctionPolygons.empty());
            if (ImGui::Button("Clear All Polygons") && currentClip) {
                applyCommand(shellState, jcut::ClearCorrectionPolygonsCommand{currentClip->id});
            }
            ImGui::EndDisabled();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Titles")) {
            drawInspectorHeading("Titles", snapshot, currentClip);
            if (ImGui::Button("Create Title At Playhead")) {
                const jcut::CommandResult result = applyCommand(
                    shellState,
                    jcut::CreateTitleClipCommand{
                        snapshot.transport.currentFrame,
                        jcut::kEditorDefaultTitleDurationFrames});
                if (result.applied) {
                    shellState->titleDraftClipId = -1;
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled("Titles lane");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Uses the unnumbered Titles track first. Overlaps route "
                    "to a free Titles track or create a numbered lane.");
            }
            ImGui::Separator();
            if (!currentClip) {
                shellState->titleDraftClipId = -1;
            }
            if (currentClip && shellState->titleDraftClipId != currentClip->id) {
                jcut::EditorTitleKeyframe draft;
                draft.frame = currentClipLocalFrame;
                const auto keyframeAtPlayhead = std::find_if(
                    currentClip->titleKeyframes.begin(),
                    currentClip->titleKeyframes.end(),
                    [&](const jcut::EditorTitleKeyframe& keyframe) {
                        return keyframe.frame == currentClipLocalFrame;
                    });
                if (keyframeAtPlayhead != currentClip->titleKeyframes.end()) {
                    draft = *keyframeAtPlayhead;
                }
                hydrateTitleDraft(shellState, currentClip->id, draft);
            }
            ImGui::BeginDisabled(!currentClip);
            ImGui::InputTextMultiline("Title Text",
                                      &shellState->titleDraft.text,
                                      ImVec2(-1.0f, 90.0f));
            ImGui::DragScalar("X",
                              ImGuiDataType_Double,
                              &shellState->titleDraft.translationX,
                              1.0f,
                              nullptr,
                              nullptr,
                              "%.1f");
            ImGui::DragScalar("Y",
                              ImGuiDataType_Double,
                              &shellState->titleDraft.translationY,
                              1.0f,
                              nullptr,
                              nullptr,
                              "%.1f");
            const double minimumFontSize = 8.0;
            const double maximumFontSize = 240.0;
            ImGui::SliderScalar("Font Size",
                                ImGuiDataType_Double,
                                &shellState->titleDraft.fontSize,
                                &minimumFontSize,
                                &maximumFontSize,
                                "%.0f");
            const double minimumOpacity = 0.0;
            const double maximumOpacity = 1.0;
            ImGui::SliderScalar("Title Opacity",
                                ImGuiDataType_Double,
                                &shellState->titleDraft.opacity,
                                &minimumOpacity,
                                &maximumOpacity,
                                "%.2f");
            inputTextForString<128>("Font Family", &shellState->titleDraft.fontFamily);
            ImGui::Checkbox("Bold", &shellState->titleDraft.bold);
            ImGui::SameLine();
            ImGui::Checkbox("Italic", &shellState->titleDraft.italic);
            editHexRgbColor("Title Color",
                            &shellState->titleDraft.color,
                            "#ffffff");
            ImGui::Checkbox("Auto Fit To Output",
                            &shellState->titleDraft.autoFitToOutput);
            inputTextForString<512>("Logo Path", &shellState->titleDraft.logoPath);
            const auto materialCombo = [](const char* label, std::string* value) {
                const std::array<std::pair<const char*, const char*>, 5> options{{
                    {"Solid", "solid"},
                    {"Neon", "neon"},
                    {"Diagonal Stripes", "diagonal_stripes"},
                    {"Grid", "grid"},
                    {"Image Pattern", "image_pattern"},
                }};
                const char* preview = "Solid";
                for (const auto& [name, stored] : options) {
                    if (*value == stored) preview = name;
                }
                bool changed = false;
                if (ImGui::BeginCombo(label, preview)) {
                    for (const auto& [name, stored] : options) {
                        const bool selected = *value == stored;
                        if (ImGui::Selectable(name, selected)) {
                            *value = stored;
                            changed = true;
                        }
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                return changed;
            };
            const auto sliderDouble = [](const char* label, double* value,
                                         double minimum, double maximum,
                                         const char* format) {
                return ImGui::SliderScalar(label, ImGuiDataType_Double, value,
                                           &minimum, &maximum, format);
            };
            ImGui::SeparatorText("Material");
            materialCombo("Text Material", &shellState->titleDraft.textMaterialStyle);
            ImGui::BeginDisabled(shellState->titleDraft.textMaterialStyle != "image_pattern");
            inputTextForString<512>("Text Pattern Path",
                                    &shellState->titleDraft.textPatternImagePath);
            sliderDouble("Text Pattern Scale",
                         &shellState->titleDraft.textPatternScale,
                         0.1, 8.0, "%.2f");
            ImGui::EndDisabled();

            ImGui::SeparatorText("Shadow");
            ImGui::Checkbox("Drop Shadow", &shellState->titleDraft.dropShadowEnabled);
            ImGui::BeginDisabled(!shellState->titleDraft.dropShadowEnabled);
            editHexRgbColor("Shadow Color", &shellState->titleDraft.dropShadowColor,
                            "#000000");
            sliderDouble("Shadow Opacity", &shellState->titleDraft.dropShadowOpacity,
                         0.0, 1.0, "%.2f");
            sliderDouble("Shadow X", &shellState->titleDraft.dropShadowOffsetX,
                         -200.0, 200.0, "%.1f px");
            sliderDouble("Shadow Y", &shellState->titleDraft.dropShadowOffsetY,
                         -200.0, 200.0, "%.1f px");
            ImGui::EndDisabled();

            ImGui::SeparatorText("Window");
            ImGui::Checkbox("Title Window", &shellState->titleDraft.windowEnabled);
            ImGui::BeginDisabled(!shellState->titleDraft.windowEnabled);
            editHexRgbColor("Window Color", &shellState->titleDraft.windowColor,
                            "#000000");
            sliderDouble("Window Opacity", &shellState->titleDraft.windowOpacity,
                         0.0, 1.0, "%.2f");
            sliderDouble("Window Padding", &shellState->titleDraft.windowPadding,
                         0.0, 400.0, "%.1f px");
            sliderDouble("Window Width", &shellState->titleDraft.windowWidth,
                         0.0, 3840.0, "%.1f px");
            ImGui::EndDisabled();
            ImGui::Checkbox("Window Frame", &shellState->titleDraft.windowFrameEnabled);
            ImGui::BeginDisabled(!shellState->titleDraft.windowFrameEnabled);
            editHexRgbColor("Window Frame Color",
                            &shellState->titleDraft.windowFrameColor, "#ffffff");
            sliderDouble("Window Frame Opacity",
                         &shellState->titleDraft.windowFrameOpacity,
                         0.0, 1.0, "%.2f");
            sliderDouble("Window Frame Width",
                         &shellState->titleDraft.windowFrameWidth,
                         0.0, 120.0, "%.1f px");
            sliderDouble("Window Frame Gap",
                         &shellState->titleDraft.windowFrameGap,
                         0.0, 200.0, "%.1f px");
            materialCombo("Window Frame Material",
                          &shellState->titleDraft.windowFrameMaterialStyle);
            ImGui::BeginDisabled(
                shellState->titleDraft.windowFrameMaterialStyle != "image_pattern");
            inputTextForString<512>("Window Frame Pattern Path",
                                    &shellState->titleDraft.windowFramePatternImagePath);
            sliderDouble("Window Frame Pattern Scale",
                         &shellState->titleDraft.windowFramePatternScale,
                         0.1, 8.0, "%.2f");
            ImGui::EndDisabled();
            ImGui::EndDisabled();

            ImGui::SeparatorText("3D / Extrusion");
            ImGui::Checkbox("3D Transform", &shellState->titleDraft.vulkan3DEnabled);
            ImGui::Checkbox("3D Extrusion", &shellState->titleDraft.vulkan3DExtrudeEnabled);
            const std::array<std::pair<const char*, const char*>, 3> extrudeModes{{
                {"None", "none"},
                {"Stacked Copies", "stacked_copies"},
                {"Eroded Solid", "eroded_solid"},
            }};
            const char* extrudePreview = "None";
            for (const auto& [name, stored] : extrudeModes) {
                if (shellState->titleDraft.textExtrudeMode == stored) extrudePreview = name;
            }
            if (ImGui::BeginCombo("Extrusion Mode", extrudePreview)) {
                for (const auto& [name, stored] : extrudeModes) {
                    const bool selected = shellState->titleDraft.textExtrudeMode == stored;
                    if (ImGui::Selectable(name, selected)) {
                        shellState->titleDraft.textExtrudeMode = stored;
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::BeginDisabled(!shellState->titleDraft.vulkan3DExtrudeEnabled ||
                                 shellState->titleDraft.textExtrudeMode == "none");
            sliderDouble("Extrusion Depth",
                         &shellState->titleDraft.vulkan3DExtrudeDepth,
                         0.0, 2.0, "%.2f");
            sliderDouble("Bevel Scale", &shellState->titleDraft.vulkan3DBevelScale,
                         0.0, 2.0, "%.2f");
            ImGui::EndDisabled();
            ImGui::BeginDisabled(!shellState->titleDraft.vulkan3DEnabled);
            sliderDouble("Yaw", &shellState->titleDraft.vulkan3DYawDegrees,
                         -360.0, 360.0, "%.1f deg");
            sliderDouble("Pitch", &shellState->titleDraft.vulkan3DPitchDegrees,
                         -360.0, 360.0, "%.1f deg");
            sliderDouble("Roll", &shellState->titleDraft.vulkan3DRollDegrees,
                         -360.0, 360.0, "%.1f deg");
            sliderDouble("3D Depth", &shellState->titleDraft.vulkan3DDepth,
                         -10.0, 10.0, "%.2f");
            sliderDouble("3D Scale", &shellState->titleDraft.vulkan3DScale,
                         0.01, 10.0, "%.2f");
            ImGui::EndDisabled();
            ImGui::Checkbox("Linear Interpolation",
                            &shellState->titleDraft.linearInterpolation);
            if (ImGui::Button("Add/Update At Playhead") && currentClip) {
                jcut::EditorTitleKeyframe keyframe = shellState->titleDraft;
                keyframe.frame = currentClipLocalFrame;
                applyCommand(shellState, jcut::UpsertTitleKeyframeCommand{
                    currentClip->id, std::move(keyframe)});
            }
            ImGui::SameLine();
            if (ImGui::Button("Remove At Playhead") && currentClip) {
                applyCommand(shellState, jcut::RemoveTitleKeyframeCommand{
                    currentClip->id, currentClipLocalFrame});
            }
            ImGui::EndDisabled();
            if (ImGui::BeginTable("TitleKeys", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Frame");
                ImGui::TableSetupColumn("Text");
                ImGui::TableSetupColumn("Position");
                ImGui::TableSetupColumn("Style");
                ImGui::TableSetupColumn("Interp");
                ImGui::TableSetupColumn("Actions");
                ImGui::TableHeadersRow();
                if (currentClip) {
                    for (const jcut::EditorTitleKeyframe& keyframe : currentClip->titleKeyframes) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        if (drawFrameSeekCell(
                                shellState,
                                keyframe.frame,
                                static_cast<std::int64_t>(currentClip->startFrame) + keyframe.frame,
                                "title-frame-" + std::to_string(keyframe.frame))) {
                            hydrateTitleDraft(shellState, currentClip->id, keyframe);
                        }
                        ImGui::TableNextColumn();
                        ImGui::TextWrapped("%s", keyframe.text.c_str());
                        ImGui::TableNextColumn();
                        ImGui::Text("%.0f, %.0f", keyframe.translationX, keyframe.translationY);
                        ImGui::TableNextColumn();
                        ImGui::TextWrapped("%s %.0f | %.2f | %s%s | %s",
                                           keyframe.fontFamily.c_str(),
                                           keyframe.fontSize,
                                           keyframe.opacity,
                                           keyframe.bold ? "Bold" : "Regular",
                                           keyframe.italic ? " Italic" : "",
                                           keyframe.color.c_str());
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(
                            keyframe.linearInterpolation ? "Linear" : "Hold");
                        ImGui::TableNextColumn();
                        const std::string keyId = "title-" +
                            std::to_string(keyframe.frame);
                        ImGui::PushID(keyId.c_str());
                        if (ImGui::SmallButton("Load")) {
                            hydrateTitleDraft(shellState, currentClip->id, keyframe);
                            applyCommand(shellState, jcut::SeekToFrameCommand{
                                static_cast<int>(std::clamp<std::int64_t>(
                                    static_cast<std::int64_t>(currentClip->startFrame) + keyframe.frame,
                                    0,
                                    std::numeric_limits<int>::max()))});
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton(
                                keyframe.linearInterpolation
                                    ? "Set Hold"
                                    : "Set Linear")) {
                            jcut::EditorTitleKeyframe updated = keyframe;
                            updated.linearInterpolation = !updated.linearInterpolation;
                            applyCommand(shellState, jcut::UpsertTitleKeyframeCommand{
                                currentClip->id, std::move(updated)});
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Remove")) {
                            applyCommand(shellState, jcut::RemoveTitleKeyframeCommand{
                                currentClip->id, keyframe.frame});
                        }
                        ImGui::PopID();
                    }
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Sync")) {
            drawInspectorHeading("Sync", snapshot, currentClip);
            ImGui::BeginDisabled(!currentClip);
            if (ImGui::Button("Duplicate Frame") && currentClip) {
                applyCommand(shellState, jcut::AddRenderSyncMarkerCommand{
                    currentClip->id, snapshot.transport.currentFrame, false, 1});
            }
            ImGui::SameLine();
            if (ImGui::Button("Skip Frame") && currentClip) {
                applyCommand(shellState, jcut::AddRenderSyncMarkerCommand{
                    currentClip->id, snapshot.transport.currentFrame, true, 1});
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(snapshot.renderSyncMarkers.empty());
            if (ImGui::Button("Clear All Sync Points")) {
                applyCommand(shellState, jcut::ClearRenderSyncMarkersCommand{});
            }
            ImGui::EndDisabled();
            if (ImGui::BeginTable("SyncPoints", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Clip");
                ImGui::TableSetupColumn("Frame");
                ImGui::TableSetupColumn("Count");
                ImGui::TableSetupColumn("Operation");
                ImGui::TableSetupColumn("Actions");
                ImGui::TableHeadersRow();
                for (std::size_t markerIndex = 0;
                     markerIndex < snapshot.renderSyncMarkers.size();
                     ++markerIndex) {
                    const jcut::EditorRenderSyncMarker& marker =
                        snapshot.renderSyncMarkers[markerIndex];
                    const jcut::EditorClip* owner =
                        clipForPersistentId(snapshot, marker.clipId);
                    ImGui::TableNextRow();
                    ImGui::PushID(static_cast<int>(markerIndex));
                    ImGui::TableNextColumn();
                    ImGui::TextWrapped("%s", marker.clipId.c_str());
                    ImGui::TableNextColumn();
                    std::int64_t frame = marker.frame;
                    ImGui::SetNextItemWidth(84.0f);
                    ImGui::BeginDisabled(!owner);
                    const bool frameChanged = ImGui::InputScalar(
                        "##frame",
                        ImGuiDataType_S64,
                        &frame,
                        nullptr,
                        nullptr,
                        "%lld",
                        ImGuiInputTextFlags_EnterReturnsTrue);
                    ImGui::EndDisabled();
                    if (frameChanged && owner) {
                        replaceRenderSyncMarker(
                            shellState,
                            marker,
                            owner->id,
                            frame,
                            marker.skipFrame,
                            marker.count);
                    }
                    ImGui::TableNextColumn();
                    int count = marker.count;
                    ImGui::SetNextItemWidth(60.0f);
                    ImGui::BeginDisabled(!owner);
                    const bool countChanged = ImGui::InputInt(
                        "##count",
                        &count,
                        1,
                        10,
                        ImGuiInputTextFlags_EnterReturnsTrue);
                    ImGui::EndDisabled();
                    if (countChanged && owner) {
                        replaceRenderSyncMarker(
                            shellState,
                            marker,
                            owner->id,
                            marker.frame,
                            marker.skipFrame,
                            std::clamp(
                                count,
                                jcut::kEditorRenderSyncMinCount,
                                jcut::kEditorRenderSyncMaxCount));
                    }
                    ImGui::TableNextColumn();
                    constexpr const char* operations[] = {"Duplicate", "Skip"};
                    int operation = marker.skipFrame ? 1 : 0;
                    ImGui::SetNextItemWidth(92.0f);
                    ImGui::BeginDisabled(!owner);
                    const bool operationChanged = ImGui::Combo(
                        "##operation", &operation, operations, IM_ARRAYSIZE(operations));
                    ImGui::EndDisabled();
                    if (operationChanged && owner) {
                        replaceRenderSyncMarker(
                            shellState,
                            marker,
                            owner->id,
                            marker.frame,
                            operation == 1,
                            marker.count);
                    }
                    ImGui::TableNextColumn();
                    if (ImGui::SmallButton("Seek")) {
                        applyCommand(shellState,
                                     jcut::SeekToFrameCommand{
                                         static_cast<int>(std::clamp<std::int64_t>(
                                             marker.frame,
                                             0,
                                             std::numeric_limits<int>::max()))});
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Remove")) {
                        applyCommand(shellState,
                                     jcut::RemoveRenderSyncMarkerCommand{
                                         marker.clipId,
                                         marker.frame,
                                         marker.skipFrame});
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Transform")) {
            drawInspectorHeading("Transform", snapshot, currentClip);
            float tx = currentClip ? static_cast<float>(currentClip->baseTranslationX) : 0.0f;
            float ty = currentClip ? static_cast<float>(currentClip->baseTranslationY) : 0.0f;
            float rotation = currentClip ? static_cast<float>(currentClip->baseRotation) : 0.0f;
            float scaleX = currentClip ? static_cast<float>(currentClip->baseScaleX) : 1.0f;
            float scaleY = currentClip ? static_cast<float>(currentClip->baseScaleY) : 1.0f;
            ImGui::BeginDisabled(!currentClip);
            bool transformChanged = false;
            transformChanged |= ImGui::DragFloat("Translate X", &tx, 1.0f);
            beginRuntimeHistoryTransactionForLastItem(shellState);
            transformChanged |= ImGui::DragFloat("Translate Y", &ty, 1.0f);
            beginRuntimeHistoryTransactionForLastItem(shellState);
            transformChanged |= ImGui::SliderFloat("Rotation", &rotation, -180.0f, 180.0f, "%.1f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            transformChanged |= ImGui::SliderFloat("Scale X", &scaleX, -4.0f, 4.0f, "%.2f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            transformChanged |= ImGui::SliderFloat("Scale Y", &scaleY, -4.0f, 4.0f, "%.2f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            if (transformChanged && currentClip) {
                applyCommand(shellState, jcut::SetClipTransformCommand{
                    currentClip->id, tx, ty, rotation, scaleX, scaleY});
            }
            if (ImGui::Button("Flip Horizontal") && currentClip) {
                applyCommand(shellState, jcut::SetClipTransformCommand{
                    currentClip->id, tx, ty, rotation, -scaleX, scaleY});
            }
            const jcut::EditorTransformKeyframe transformInitial{
                currentClipLocalFrame,
                {},
                tx,
                ty,
                rotation,
                scaleX,
                scaleY,
                true};
            drawKeyframeDraftEditor<
                jcut::EditorTransformKeyframe,
                jcut::UpsertTransformKeyframeCommand>(
                    shellState,
                    currentClip,
                    jcut::EditorKeyframeChannel::Transform,
                    currentClipLastFrame,
                    "TransformKeyframeEditor",
                    transformInitial,
                    [](jcut::EditorTransformKeyframe* draft) {
                        inputTextForString<128>(
                            "Transform Title", &draft->title);
                        ImGui::InputDouble(
                            "Key Translate X", &draft->translationX, 1.0, 10.0, "%.3f");
                        ImGui::InputDouble(
                            "Key Translate Y", &draft->translationY, 1.0, 10.0, "%.3f");
                        ImGui::InputDouble(
                            "Key Rotation", &draft->rotation, 0.1, 1.0, "%.3f");
                        ImGui::InputDouble(
                            "Key Scale X", &draft->scaleX, 0.01, 0.1, "%.3f");
                        ImGui::InputDouble(
                            "Key Scale Y", &draft->scaleY, 0.01, 0.1, "%.3f");
                    });
            ImGui::EndDisabled();
            if (ImGui::BeginTable(
                    "TransformKeys",
                    7,
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                        ImGuiTableFlags_ScrollX)) {
                ImGui::TableSetupColumn("Frame");
                ImGui::TableSetupColumn("Title");
                ImGui::TableSetupColumn("Position");
                ImGui::TableSetupColumn("Rotation");
                ImGui::TableSetupColumn("Scale");
                ImGui::TableSetupColumn("Interp");
                ImGui::TableSetupColumn("Actions");
                ImGui::TableHeadersRow();
                if (currentClip) {
                    for (const jcut::EditorTransformKeyframe& keyframe : currentClip->transformKeyframes) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        drawFrameSeekCell(
                            shellState,
                            keyframe.frame,
                            static_cast<std::int64_t>(currentClip->startFrame) + keyframe.frame,
                            "transform-frame-" + std::to_string(keyframe.frame));
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(
                            keyframe.title.empty() ? "-" : keyframe.title.c_str());
                        ImGui::TableNextColumn();
                        ImGui::Text("%.0f, %.0f", keyframe.translationX, keyframe.translationY);
                        ImGui::TableNextColumn();
                        ImGui::Text("%.1f", keyframe.rotation);
                        ImGui::TableNextColumn();
                        ImGui::Text("%.2f, %.2f", keyframe.scaleX, keyframe.scaleY);
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(
                            keyframe.linearInterpolation ? "Linear" : "Hold");
                        ImGui::TableNextColumn();
                        const std::string keyId = "transform-" +
                            std::to_string(keyframe.frame);
                        ImGui::PushID(keyId.c_str());
                        if (ImGui::SmallButton("Load/Edit")) {
                            loadKeyframeDraft(
                                shellState,
                                currentClip->id,
                                jcut::EditorKeyframeChannel::Transform,
                                keyframe);
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton(
                                keyframe.linearInterpolation
                                    ? "Set Hold"
                                    : "Set Linear")) {
                            jcut::EditorTransformKeyframe updated = keyframe;
                            updated.linearInterpolation =
                                !updated.linearInterpolation;
                            applyCommand(shellState,
                                         jcut::UpsertTransformKeyframeCommand{
                                             currentClip->id,
                                             std::move(updated)});
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Remove")) {
                            removeInspectorKeyframe(
                                shellState,
                                currentClip->id,
                                jcut::EditorKeyframeChannel::Transform,
                                keyframe.frame);
                        }
                        ImGui::PopID();
                    }
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Transcript")) {
            drawInspectorHeading("Transcript", snapshot, currentClip);
            jcut::EditorTranscriptOverlayState overlay = currentClip
                ? currentClip->transcriptOverlay
                : jcut::EditorTranscriptOverlayState{};
            float width = static_cast<float>(overlay.boxWidth);
            float height = static_cast<float>(overlay.boxHeight);
            float translationX = static_cast<float>(overlay.translationX);
            float translationY = static_cast<float>(overlay.translationY);
            float textOpacity = static_cast<float>(overlay.textOpacity);
            float backgroundOpacity = static_cast<float>(overlay.backgroundOpacity);
            float backgroundCornerRadius = static_cast<float>(overlay.backgroundCornerRadius);
            float backgroundPadding = static_cast<float>(overlay.backgroundPadding);
            float backgroundFrameOpacity = static_cast<float>(overlay.backgroundFrameOpacity);
            float backgroundFrameWidth = static_cast<float>(overlay.backgroundFrameWidth);
            float backgroundFrameGap = static_cast<float>(overlay.backgroundFrameGap);
            float shadowOpacity = static_cast<float>(overlay.shadowOpacity);
            float shadowOffsetX = static_cast<float>(overlay.shadowOffsetX);
            float shadowOffsetY = static_cast<float>(overlay.shadowOffsetY);
            float textOutlineWidth = static_cast<float>(overlay.textOutlineWidth);
            float textOutlineOpacity = static_cast<float>(overlay.textOutlineOpacity);
            float textExtrudeDepth = static_cast<float>(overlay.textExtrudeDepth);
            float textExtrudeBevelScale = static_cast<float>(overlay.textExtrudeBevelScale);
            int fontPointSize = overlay.fontPointSize;
            std::array<char, 128> fontFamily{};
            std::snprintf(fontFamily.data(), fontFamily.size(), "%s",
                          overlay.fontFamily.c_str());
            ImGui::BeginDisabled(!currentClip);
            bool overlayChanged = false;
            overlayChanged |= ImGui::Checkbox("Enable Overlay", &overlay.enabled);
            overlayChanged |= ImGui::Checkbox("Manual Placement", &overlay.useManualPlacement);
            ImGui::BeginDisabled(!overlay.useManualPlacement);
            overlayChanged |= ImGui::SliderFloat(
                "Center X", &translationX, -1.0f, 1.0f, "%.3f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            overlayChanged |= ImGui::SliderFloat(
                "Center Y", &translationY, -1.0f, 1.0f, "%.3f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            ImGui::EndDisabled();
            overlayChanged |= ImGui::InputInt("Max Lines", &overlay.maxLines);
            beginRuntimeHistoryTransactionForLastItem(shellState);
            overlayChanged |= ImGui::InputInt("Max Chars", &overlay.maxCharsPerLine);
            beginRuntimeHistoryTransactionForLastItem(shellState);
            overlayChanged |= ImGui::DragFloat("Width", &width, 4.0f, 160.0f, 3840.0f, "%.0f px");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            overlayChanged |= ImGui::DragFloat("Height", &height, 4.0f, 80.0f, 2160.0f, "%.0f px");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            overlayChanged |= ImGui::SliderFloat("Text Opacity", &textOpacity, 0.0f, 1.0f, "%.2f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            const bool fontFamilyChanged = ImGui::InputText(
                "Font Family", fontFamily.data(), fontFamily.size());
            beginRuntimeHistoryTransactionForLastItem(shellState);
            overlayChanged |= fontFamilyChanged;
            overlayChanged |= ImGui::SliderInt(
                "Font Size", &fontPointSize, 12, 256, "%d pt");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            overlayChanged |= ImGui::Checkbox("Bold", &overlay.bold);
            overlayChanged |= ImGui::Checkbox("Italic", &overlay.italic);
            overlayChanged |= editHexRgbColor(
                "Text Color", &overlay.textColor, "#ffffff");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            overlayChanged |= ImGui::Checkbox("Show Background", &overlay.showBackground);
            overlayChanged |= ImGui::SliderFloat("Background Opacity", &backgroundOpacity, 0.0f, 1.0f, "%.2f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            overlayChanged |= editHexRgbColor(
                "Background Color", &overlay.backgroundColor, "#000000");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            overlayChanged |= ImGui::SliderFloat(
                "Background Radius", &backgroundCornerRadius, 0.0f, 128.0f, "%.1f px");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            overlayChanged |= ImGui::SliderFloat(
                "Content Padding", &backgroundPadding, 0.0f, 400.0f, "%.1f px");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            overlayChanged |= ImGui::Checkbox(
                "Background Frame", &overlay.backgroundFrameEnabled);
            ImGui::BeginDisabled(!overlay.backgroundFrameEnabled);
            overlayChanged |= editHexRgbColor(
                "Frame Color", &overlay.backgroundFrameColor, "#ffffff");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            overlayChanged |= ImGui::SliderFloat(
                "Frame Opacity", &backgroundFrameOpacity, 0.0f, 1.0f, "%.2f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            overlayChanged |= ImGui::SliderFloat(
                "Frame Width", &backgroundFrameWidth, 0.0f, 120.0f, "%.1f px");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            overlayChanged |= ImGui::SliderFloat(
                "Frame Gap", &backgroundFrameGap, 0.0f, 200.0f, "%.1f px");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            ImGui::EndDisabled();
            overlayChanged |= ImGui::Checkbox("Show Shadow", &overlay.showShadow);
            ImGui::BeginDisabled(!overlay.showShadow);
            overlayChanged |= editHexRgbColor(
                "Shadow Color", &overlay.shadowColor, "#000000");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            overlayChanged |= ImGui::SliderFloat(
                "Shadow Opacity", &shadowOpacity, 0.0f, 1.0f, "%.2f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            overlayChanged |= ImGui::SliderFloat(
                "Shadow X", &shadowOffsetX, -128.0f, 128.0f, "%.1f px");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            overlayChanged |= ImGui::SliderFloat(
                "Shadow Y", &shadowOffsetY, -128.0f, 128.0f, "%.1f px");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            ImGui::EndDisabled();
            overlayChanged |= ImGui::Checkbox("Text Outline", &overlay.textOutlineEnabled);
            ImGui::BeginDisabled(!overlay.textOutlineEnabled);
            overlayChanged |= editHexRgbColor(
                "Outline Color", &overlay.textOutlineColor, "#000000");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            overlayChanged |= ImGui::SliderFloat(
                "Outline Width", &textOutlineWidth, 0.0f, 24.0f, "%.1f px");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            overlayChanged |= ImGui::SliderFloat(
                "Outline Opacity", &textOutlineOpacity, 0.0f, 1.0f, "%.2f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            ImGui::EndDisabled();
            const char* extrudeLabel = overlay.textExtrudeMode == "stacked_copies"
                ? "Stacked Copies" : (overlay.textExtrudeMode == "eroded_solid"
                    ? "Eroded Solid" : "None");
            if (ImGui::BeginCombo("Text Extrusion", extrudeLabel)) {
                const std::array<std::pair<const char*, const char*>, 3> modes{{
                    {"None", "none"},
                    {"Stacked Copies", "stacked_copies"},
                    {"Eroded Solid", "eroded_solid"},
                }};
                for (const auto& [label, value] : modes) {
                    const bool selected = overlay.textExtrudeMode == value;
                    if (ImGui::Selectable(label, selected)) {
                        overlay.textExtrudeMode = value;
                        overlayChanged = true;
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::BeginDisabled(overlay.textExtrudeMode == "none");
            overlayChanged |= ImGui::SliderFloat(
                "Extrude Depth", &textExtrudeDepth, 0.0f, 2.0f, "%.2f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            overlayChanged |= ImGui::SliderFloat(
                "Extrude Bevel", &textExtrudeBevelScale, 0.0f, 2.0f, "%.2f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            ImGui::EndDisabled();
            overlayChanged |= ImGui::Checkbox("Show Speaker Title", &overlay.showSpeakerTitle);
            overlayChanged |= ImGui::Checkbox("Highlight Current Word", &overlay.highlightCurrentWord);
            overlayChanged |= editHexRgbColor(
                "Highlight Color", &overlay.highlightColor, "#fff2a8");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            overlayChanged |= editHexRgbColor(
                "Highlight Text Color", &overlay.highlightTextColor, "#181818");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            overlayChanged |= ImGui::Checkbox("Auto Scroll", &overlay.autoScroll);
            if (overlayChanged && currentClip) {
                overlay.translationX = translationX;
                overlay.translationY = translationY;
                overlay.boxWidth = width;
                overlay.boxHeight = height;
                overlay.textOpacity = textOpacity;
                overlay.backgroundOpacity = backgroundOpacity;
                overlay.backgroundCornerRadius = backgroundCornerRadius;
                overlay.backgroundPadding = backgroundPadding;
                overlay.backgroundFrameOpacity = backgroundFrameOpacity;
                overlay.backgroundFrameWidth = backgroundFrameWidth;
                overlay.backgroundFrameGap = backgroundFrameGap;
                overlay.shadowOpacity = shadowOpacity;
                overlay.shadowOffsetX = shadowOffsetX;
                overlay.shadowOffsetY = shadowOffsetY;
                overlay.textOutlineWidth = textOutlineWidth;
                overlay.textOutlineOpacity = textOutlineOpacity;
                overlay.textExtrudeDepth = textExtrudeDepth;
                overlay.textExtrudeBevelScale = textExtrudeBevelScale;
                overlay.fontPointSize = fontPointSize;
                if (fontFamilyChanged) {
                    overlay.fontFamily = fontFamily.data();
                }
                applyCommand(shellState, jcut::SetClipTranscriptOverlayCommand{
                    currentClip->id, std::move(overlay)});
            }
            ImGui::EndDisabled();
            ImGui::SeparatorText("Transcript Document");
            if (!currentClip) {
                ImGui::TextWrapped("Select an audio clip to inspect its transcript.");
            } else if (currentClip->mediaKind != "audio" && !currentClip->hasAudio) {
                ImGui::TextWrapped("The selected clip has no detected audio stream.");
            } else {
                ensureTranscriptInspectorCache(shellState, snapshot, *currentClip);
                TranscriptInspectorCache& cache = shellState->transcriptCache;
                const jcut::TranscriptCutSession& transcript = cache.session;

                std::string activeLabel = "No transcript";
                for (const jcut::TranscriptCutEntry& cut : transcript.catalog.cuts) {
                    if (cut.path == transcript.activePath) {
                        activeLabel = cut.label;
                        break;
                    }
                }
                ImGui::BeginDisabled(transcript.catalog.cuts.empty());
                if (ImGui::BeginCombo("Cut", activeLabel.c_str())) {
                    for (const jcut::TranscriptCutEntry& cut : transcript.catalog.cuts) {
                        const bool selected = cut.path == transcript.activePath;
                        if (ImGui::Selectable(cut.label.c_str(), selected)) {
                            applyCommand(
                                shellState,
                                jcut::SetClipTranscriptActiveCutCommand{
                                    currentClip->id, cut.path});
                            cache.requestedPath = cut.path;
                            cache.refreshRequested = true;
                        }
                        if (selected) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::EndDisabled();
                if (cache.cutLabelPath != transcript.activePath) {
                    cache.cutLabelPath = transcript.activePath;
                    cache.cutLabelDraft = activeLabel;
                }
                ImGui::BeginDisabled(!transcript.ok());
                if (ImGui::Button("New Cut")) {
                    std::string error;
                    const std::optional<std::string> newPath =
                        jcut::createTranscriptCutVersion(transcript, &error);
                    if (newPath) {
                        applyCommand(
                            shellState,
                            jcut::SetClipTranscriptActiveCutCommand{
                                currentClip->id, *newPath});
                        cache.requestedPath = *newPath;
                        cache.refreshRequested = true;
                    } else {
                        cache.mutationError = std::move(error);
                    }
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::SetNextItemWidth(
                    std::max(120.0f, ImGui::GetContentRegionAvail().x - 190.0f));
                ImGui::InputText("Cut Label", &cache.cutLabelDraft);
                ImGui::SameLine();
                ImGui::BeginDisabled(!transcript.activeCutMutable);
                if (ImGui::Button("Rename")) {
                    std::string error;
                    if (jcut::renameTranscriptCut(
                            transcript, cache.cutLabelDraft, &error)) {
                        cache.refreshRequested = true;
                    } else if (!error.empty()) {
                        cache.mutationError = std::move(error);
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Delete Cut")) {
                    ImGui::OpenPopup("Confirm Delete Transcript Cut");
                }
                ImGui::EndDisabled();
                if (ImGui::BeginPopupModal(
                        "Confirm Delete Transcript Cut", nullptr,
                        ImGuiWindowFlags_AlwaysAutoResize)) {
                    ImGui::TextWrapped(
                        "Permanently delete \"%s\"? This cannot be undone.",
                        activeLabel.c_str());
                    if (ImGui::Button("Delete")) {
                        std::string fallback;
                        std::string error;
                        if (jcut::deleteTranscriptCut(
                                transcript, &fallback, &error)) {
                            applyCommand(
                                shellState,
                                jcut::SetClipTranscriptActiveCutCommand{
                                    currentClip->id, fallback});
                            cache.requestedPath = fallback;
                            cache.refreshRequested = true;
                            ImGui::CloseCurrentPopup();
                        } else {
                            cache.mutationError = std::move(error);
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                }

                bool showOutsideCut = legacyBoolValue(
                    *shellState, "transcriptShowExcludedLines", false);
                if (ImGui::Checkbox("Show Outside Cut", &showOutsideCut)) {
                    setLegacyStateOverride(
                        shellState,
                        "transcriptShowExcludedLines",
                        showOutsideCut);
                    cache.refreshRequested = true;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Refresh Transcript")) {
                    cache.refreshRequested = true;
                }
                ImGui::TextDisabled(transcript.activeCutMutable
                    ? "Select a row to edit its text, source timing, or skip state."
                    : "Original transcript is immutable; select an editable cut to edit rows.");
                if (!transcript.activePath.empty()) {
                    ImGui::TextWrapped("Active: %s", transcript.activePath.c_str());
                }
                if (!transcript.warning.empty()) {
                    ImGui::TextColored(
                        ImVec4(0.95f, 0.72f, 0.28f, 1.0f),
                        "%s",
                        transcript.warning.c_str());
                }
                if (!transcript.error.empty()) {
                    ImGui::TextColored(
                        ImVec4(1.0f, 0.38f, 0.38f, 1.0f),
                        "%s",
                        transcript.error.c_str());
                } else {
                    std::vector<std::string> transcriptSpeakers;
                    for (const jcut::TranscriptRow& row : transcript.rows) {
                        if (!row.gap && !row.speakerId.empty()) {
                            transcriptSpeakers.push_back(row.speakerId);
                        }
                    }
                    std::sort(transcriptSpeakers.begin(), transcriptSpeakers.end());
                    transcriptSpeakers.erase(
                        std::unique(transcriptSpeakers.begin(), transcriptSpeakers.end()),
                        transcriptSpeakers.end());
                    const std::string speakerFilterLabel = cache.speakerFilter.empty()
                        ? "All Speakers" : cache.speakerFilter;
                    if (ImGui::BeginCombo("Speaker Filter", speakerFilterLabel.c_str())) {
                        if (ImGui::Selectable("All Speakers", cache.speakerFilter.empty())) {
                            cache.speakerFilter.clear();
                        }
                        for (const std::string& speaker : transcriptSpeakers) {
                            if (ImGui::Selectable(
                                    speaker.c_str(), cache.speakerFilter == speaker)) {
                                cache.speakerFilter = speaker;
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::InputTextWithHint(
                        "Search Transcript", "Filter word text...", &cache.searchFilter);
                    const std::string normalizedSearch = lowerAscii(cache.searchFilter);
                    std::vector<const jcut::TranscriptRow*> visibleTranscriptRows;
                    visibleTranscriptRows.reserve(transcript.rows.size());
                    for (const jcut::TranscriptRow& row : transcript.rows) {
                        if (!cache.speakerFilter.empty() && !row.gap &&
                            row.speakerId != cache.speakerFilter) {
                            continue;
                        }
                        if (!normalizedSearch.empty() &&
                            (row.gap ||
                             lowerAscii(row.text).find(normalizedSearch) ==
                                 std::string::npos)) {
                            continue;
                        }
                        visibleTranscriptRows.push_back(&row);
                    }
                    const ImGuiTableFlags transcriptTableFlags =
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                        ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollX |
                        ImGuiTableFlags_ScrollY;
                    if (ImGui::BeginTable(
                            "TranscriptRows",
                            6,
                            transcriptTableFlags,
                            ImVec2(0.0f, 280.0f))) {
                        ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, 82.0f);
                        ImGui::TableSetupColumn("Frames", ImGuiTableColumnFlags_WidthFixed, 92.0f);
                        ImGui::TableSetupColumn("Render", ImGuiTableColumnFlags_WidthFixed, 92.0f);
                        ImGui::TableSetupColumn("Speaker", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                        ImGui::TableSetupColumn("Text", ImGuiTableColumnFlags_WidthStretch, 180.0f);
                        ImGui::TableSetupColumn("Edits", ImGuiTableColumnFlags_WidthFixed, 110.0f);
                        ImGui::TableHeadersRow();
                        ImGuiListClipper clipper;
                        clipper.Begin(static_cast<int>(visibleTranscriptRows.size()));
                        while (clipper.Step()) {
                            for (int rowIndex = clipper.DisplayStart;
                                 rowIndex < clipper.DisplayEnd;
                                 ++rowIndex) {
                                const jcut::TranscriptRow& row =
                                    *visibleTranscriptRows[static_cast<std::size_t>(rowIndex)];
                                ImGui::TableNextRow();
                                ImGui::TableNextColumn();
                                const std::string sourceTime =
                                    jcut::formatTranscriptTime(row.rawStartSeconds);
                                const bool rowSelected = cache.selectionDraftValid &&
                                    cache.selectedWord.segmentIndex == row.word.segmentIndex &&
                                    cache.selectedWord.wordIndex == row.word.wordIndex;
                                ImGui::PushID(rowIndex);
                                if (ImGui::Selectable(sourceTime.c_str(), rowSelected,
                                                      ImGuiSelectableFlags_SpanAllColumns |
                                                          ImGuiSelectableFlags_AllowOverlap) &&
                                    transcript.activeCutMutable) {
                                    selectTranscriptWordDraft(&cache, row);
                                }
                                ImGui::TableNextColumn();
                                ImGui::Text("%lld-%lld",
                                            static_cast<long long>(row.sourceStartFrame),
                                            static_cast<long long>(row.sourceEndFrame));
                                ImGui::TableNextColumn();
                                if (row.renderStartFrame < 0 || row.renderEndFrame < 0) {
                                    ImGui::TextUnformatted("Outside");
                                } else {
                                    ImGui::Text("%lld-%lld",
                                                static_cast<long long>(row.renderStartFrame),
                                                static_cast<long long>(row.renderEndFrame));
                                }
                                ImGui::TableNextColumn();
                                const std::string& speaker = row.speakerLabel.empty()
                                    ? row.speakerId
                                    : row.speakerLabel;
                                ImGui::TextUnformatted(speaker.c_str());
                                if (!speaker.empty() && ImGui::IsItemHovered()) {
                                    ImGui::SetTooltip("%s", speaker.c_str());
                                }
                                ImGui::TableNextColumn();
                                ImGui::TextUnformatted(row.text.c_str());
                                if (!row.text.empty() && ImGui::IsItemHovered()) {
                                    ImGui::SetTooltip("%s", row.text.c_str());
                                }
                                ImGui::TableNextColumn();
                                const std::string edits = transcriptRowEditLabels(row);
                                ImGui::TextUnformatted(edits.c_str());
                                ImGui::PopID();
                            }
                        }
                        ImGui::EndTable();
                    }
                    bool canUndoGlobally = false;
                    bool canRedoGlobally = false;
                    {
                        std::lock_guard<std::mutex> lock(shellState->runtimeMutex);
                        canUndoGlobally = shellState->runtime.canUndo();
                        canRedoGlobally = shellState->runtime.canRedo();
                    }
                    ImGui::BeginDisabled(!canUndoGlobally);
                    if (ImGui::Button("Undo Last Edit")) {
                        applyCommand(shellState, jcut::UndoCommand{});
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::BeginDisabled(!canRedoGlobally);
                    if (ImGui::Button("Redo Last Edit")) {
                        applyCommand(shellState, jcut::RedoCommand{});
                    }
                    ImGui::EndDisabled();
                    ImGui::BeginDisabled(!transcript.activeCutMutable ||
                                         !cache.selectionDraftValid);
                    ImGui::SeparatorText("Selected Word");
                    ImGui::InputDouble("Source Start (s)", &cache.selectedStartSeconds,
                                       0.01, 0.1, "%.3f");
                    ImGui::InputDouble("Source End (s)", &cache.selectedEndSeconds,
                                       0.01, 0.1, "%.3f");
                    ImGui::InputTextMultiline("Text", &cache.selectedText,
                                              ImVec2(-FLT_MIN, 64.0f));
                    ImGui::Checkbox("Skipped", &cache.selectedSkipped);
                    if (ImGui::Button("Save Word") && transcript.activeDocument) {
                        nlohmann::json root = transcript.activeDocument->root();
                        jcut::TranscriptWordPatch patch;
                        patch.startSeconds = cache.selectedStartSeconds;
                        patch.endSeconds = cache.selectedEndSeconds;
                        patch.text = cache.selectedText;
                        patch.skipped = cache.selectedSkipped;
                        std::string error;
                        if (jcut::patchTranscriptWord(
                                &root, cache.selectedWord, patch, &error)) {
                            saveTranscriptMutation(shellState, &cache, std::move(root));
                        } else if (!error.empty()) {
                            cache.mutationError = std::move(error);
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Add Above") && transcript.activeDocument) {
                        nlohmann::json root = transcript.activeDocument->root();
                        std::string error;
                        if (jcut::insertTranscriptWord(
                                &root, cache.selectedWord, true, &error)) {
                            saveTranscriptMutation(shellState, &cache, std::move(root));
                        } else {
                            cache.mutationError = std::move(error);
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Add Below") && transcript.activeDocument) {
                        nlohmann::json root = transcript.activeDocument->root();
                        std::string error;
                        if (jcut::insertTranscriptWord(
                                &root, cache.selectedWord, false, &error)) {
                            saveTranscriptMutation(shellState, &cache, std::move(root));
                        } else {
                            cache.mutationError = std::move(error);
                        }
                    }
                    if (ImGui::Button("Expand Timing") && transcript.activeDocument) {
                        nlohmann::json root = transcript.activeDocument->root();
                        std::string error;
                        if (jcut::expandTranscriptWordTiming(
                                &root, cache.selectedWord, &error)) {
                            saveTranscriptMutation(shellState, &cache, std::move(root));
                        } else if (!error.empty()) {
                            cache.mutationError = std::move(error);
                        }
                    }
                    ImGui::SameLine();
                    ImGui::BeginDisabled(
                        cache.selectedWord.originalSegmentIndex < 0 ||
                        cache.selectedWord.originalWordIndex < 0 ||
                        !transcript.originalDocument);
                    if (ImGui::Button("Restore Original") &&
                        transcript.activeDocument && transcript.originalDocument) {
                        nlohmann::json root = transcript.activeDocument->root();
                        std::string error;
                        if (jcut::restoreTranscriptWord(
                                &root,
                                cache.selectedWord,
                                transcript.originalDocument->root(),
                                &error)) {
                            saveTranscriptMutation(shellState, &cache, std::move(root));
                        } else if (!error.empty()) {
                            cache.mutationError = std::move(error);
                        }
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    if (ImGui::Button("Move Up") && transcript.activeDocument) {
                        nlohmann::json root = transcript.activeDocument->root();
                        std::string error;
                        if (jcut::moveTranscriptWordRenderOrder(
                                &root, cache.selectedWord, -1, &error)) {
                            saveTranscriptMutation(shellState, &cache, std::move(root));
                        } else if (!error.empty()) {
                            cache.mutationError = std::move(error);
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Move Down") && transcript.activeDocument) {
                        nlohmann::json root = transcript.activeDocument->root();
                        std::string error;
                        if (jcut::moveTranscriptWordRenderOrder(
                                &root, cache.selectedWord, 1, &error)) {
                            saveTranscriptMutation(shellState, &cache, std::move(root));
                        } else if (!error.empty()) {
                            cache.mutationError = std::move(error);
                        }
                    }
                    if (ImGui::Button("Delete Word")) {
                        ImGui::OpenPopup("Confirm Delete Transcript Word");
                    }
                    ImGui::EndDisabled();
                    if (ImGui::BeginPopupModal("Confirm Delete Transcript Word", nullptr,
                                               ImGuiWindowFlags_AlwaysAutoResize)) {
                        ImGui::TextWrapped("Delete the selected word from this editable cut?");
                        if (ImGui::Button("Delete") && transcript.activeDocument) {
                            nlohmann::json root = transcript.activeDocument->root();
                            std::string error;
                            if (jcut::deleteTranscriptWord(
                                    &root, cache.selectedWord, &error)) {
                                saveTranscriptMutation(shellState, &cache, std::move(root));
                                ImGui::CloseCurrentPopup();
                            } else {
                                cache.mutationError = std::move(error);
                            }
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
                        ImGui::EndPopup();
                    }
                    if (!cache.mutationError.empty()) {
                        ImGui::TextColored(ImVec4(1.0f, 0.38f, 0.38f, 1.0f),
                                           "%s", cache.mutationError.c_str());
                    }
                }
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Speakers")) {
            drawInspectorHeading("Speakers", snapshot, currentClip);
            if (!currentClip ||
                (currentClip->mediaKind != "audio" && !currentClip->hasAudio)) {
                ImGui::TextWrapped(
                    "Select a clip with audio and an attached transcript.");
            } else {
                ensureTranscriptInspectorCache(shellState, snapshot, *currentClip);
                TranscriptInspectorCache& cache = shellState->transcriptCache;
                const jcut::TranscriptCutSession& transcript = cache.session;
                if (!transcript.ok()) {
                    ImGui::TextWrapped("%s",
                        transcript.error.empty()
                            ? "No transcript speaker roster is available."
                            : transcript.error.c_str());
                } else {
                    const std::vector<jcut::TranscriptSpeakerProfileCore> profiles =
                        transcript.activeDocument->speakerProfiles();
                    ImGui::Text("%zu speaker%s in active cut",
                                profiles.size(), profiles.size() == 1 ? "" : "s");
                    if (ImGui::BeginTable(
                            "SpeakersRoster",
                            6,
                            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                ImGuiTableFlags_Resizable)) {
                        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                        ImGui::TableSetupColumn("Name");
                        ImGui::TableSetupColumn("Organization");
                        ImGui::TableSetupColumn("Words", ImGuiTableColumnFlags_WidthFixed, 55.0f);
                        ImGui::TableSetupColumn("X", ImGuiTableColumnFlags_WidthFixed, 55.0f);
                        ImGui::TableSetupColumn("Y", ImGuiTableColumnFlags_WidthFixed, 55.0f);
                        ImGui::TableHeadersRow();
                        for (const auto& profile : profiles) {
                            ImGui::PushID(profile.id.c_str());
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            if (ImGui::Selectable(
                                    profile.id.c_str(),
                                    cache.selectedSpeakerId == profile.id,
                                    ImGuiSelectableFlags_SpanAllColumns |
                                        ImGuiSelectableFlags_AllowOverlap) &&
                                transcript.activeCutMutable) {
                                cache.selectedSpeakerId = profile.id;
                                cache.speakerNameDraft = profile.name;
                                cache.speakerOrganizationDraft = profile.organization;
                                cache.speakerXDraft = profile.x;
                                cache.speakerYDraft = profile.y;
                                cache.mutationError.clear();
                            }
                            ImGui::TableNextColumn();
                            ImGui::TextUnformatted(profile.name.empty()
                                ? profile.id.c_str() : profile.name.c_str());
                            ImGui::TableNextColumn();
                            ImGui::TextUnformatted(profile.organization.c_str());
                            ImGui::TableNextColumn();
                            ImGui::Text("%zu", profile.wordCount);
                            ImGui::TableNextColumn();
                            ImGui::Text("%.2f", profile.x);
                            ImGui::TableNextColumn();
                            ImGui::Text("%.2f", profile.y);
                            ImGui::PopID();
                        }
                        ImGui::EndTable();
                    }
                    ImGui::BeginDisabled(
                        !transcript.activeCutMutable ||
                        cache.selectedSpeakerId.empty());
                    ImGui::SeparatorText("Speaker Profile");
                    ImGui::InputText("Name", &cache.speakerNameDraft);
                    ImGui::InputText("Organization", &cache.speakerOrganizationDraft);
                    ImGui::InputDouble(
                        "Title X (0-1)", &cache.speakerXDraft, 0.01, 0.1, "%.3f");
                    ImGui::InputDouble(
                        "Title Y (0-1)", &cache.speakerYDraft, 0.01, 0.1, "%.3f");
                    if (ImGui::Button("Save Speaker Profile") &&
                        transcript.activeDocument) {
                        nlohmann::json root = transcript.activeDocument->root();
                        jcut::TranscriptSpeakerProfilePatch patch;
                        patch.name = cache.speakerNameDraft;
                        patch.organization = cache.speakerOrganizationDraft;
                        patch.x = cache.speakerXDraft;
                        patch.y = cache.speakerYDraft;
                        std::string error;
                        if (jcut::patchTranscriptSpeakerProfile(
                                &root, cache.selectedSpeakerId, patch, &error)) {
                            saveTranscriptMutation(shellState, &cache, std::move(root));
                        } else if (!error.empty()) {
                            cache.mutationError = std::move(error);
                        }
                    }
                    ImGui::EndDisabled();
                    if (!cache.mutationError.empty()) {
                        ImGui::TextColored(
                            ImVec4(1.0f, 0.38f, 0.38f, 1.0f),
                            "%s", cache.mutationError.c_str());
                    }

                    ImGui::SeparatorText("Animated Speaker Introductions");
                    const char* speakerTitleStyles[] = {
                        "Slide from left",
                        "Slide from right",
                        "Rise from bottom",
                        "Drop from top",
                        "3D wrap around speaker",
                    };
                    ImGui::Combo(
                        "Fly-in Style",
                        &cache.speakerTitleStyle,
                        speakerTitleStyles,
                        static_cast<int>(std::size(speakerTitleStyles)));
                    ImGui::SetNextItemWidth(120.0f);
                    ImGui::InputDouble(
                        "Title Seconds",
                        &cache.speakerTitleDurationSeconds,
                        0.1, 0.5, "%.2f");
                    cache.speakerTitleDurationSeconds = std::clamp(
                        cache.speakerTitleDurationSeconds, 1.0, 30.0);
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(120.0f);
                    ImGui::InputDouble(
                        "Delay Seconds",
                        &cache.speakerTitleDelaySeconds,
                        0.05, 0.25, "%.2f");
                    cache.speakerTitleDelaySeconds = std::clamp(
                        cache.speakerTitleDelaySeconds, 0.0, 10.0);
                    ImGui::SetNextItemWidth(120.0f);
                    ImGui::InputDouble(
                        "Fly Seconds",
                        &cache.speakerTitleFlySeconds,
                        0.05, 0.25, "%.2f");
                    cache.speakerTitleFlySeconds = std::clamp(
                        cache.speakerTitleFlySeconds, 0.1, 10.0);
                    ImGui::SameLine();
                    ImGui::Checkbox(
                        "Include organization",
                        &cache.speakerTitleShowOrganization);
                    if (ImGui::Button("Generate Speaker Introductions") &&
                        transcript.activeDocument) {
                        jcut::SpeakerTitleFlyInSettingsCore settings;
                        settings.style =
                            static_cast<jcut::SpeakerTitleFlyInStyleCore>(
                                std::clamp(cache.speakerTitleStyle, 0, 4));
                        settings.titleDurationFrames =
                            std::max<std::int64_t>(1, std::llround(
                                cache.speakerTitleDurationSeconds * 30.0));
                        settings.titleStartDelayFrames =
                            std::max<std::int64_t>(0, std::llround(
                                cache.speakerTitleDelaySeconds * 30.0));
                        settings.flyInFrames =
                            std::max<std::int64_t>(1, std::llround(
                                cache.speakerTitleFlySeconds * 30.0));
                        settings.flyOutFrames = settings.flyInFrames;
                        settings.showSpeakerOrganization =
                            cache.speakerTitleShowOrganization;
                        std::vector<jcut::EditorClip> generated =
                            jcut::makeSpeakerTitleClipsCore(
                                *currentClip,
                                *transcript.activeDocument,
                                0,
                                settings);
                        if (generated.empty()) {
                            shellState->statusMessage =
                                "no speaker changes were found in the selected transcript range";
                        } else {
                            applyCommand(
                                shellState,
                                jcut::ReplaceSpeakerTitleClipsCommand{
                                    currentClip->id,
                                    std::move(generated)});
                        }
                    }

                    ImGui::SeparatorText("Transcript Mining");
                    const auto setMiningProposals =
                        [&](std::string label,
                            std::vector<jcut::TranscriptMiningProposal> proposals) {
                            cache.miningProposalLabel = std::move(label);
                            cache.miningProposals = std::move(proposals);
                            cache.miningProposalSelected.assign(
                                cache.miningProposals.size(), 1);
                        };
                    if (ImGui::Button("Find Speaker Names") &&
                        transcript.activeDocument) {
                        setMiningProposals(
                            "Speaker name candidates",
                            jcut::mineTranscriptSpeakerNames(
                                transcript.activeDocument->root()));
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Find Organizations") &&
                        transcript.activeDocument) {
                        setMiningProposals(
                            "Organization candidates",
                            jcut::mineTranscriptOrganizations(
                                transcript.activeDocument->root()));
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Clean Spurious Labels") &&
                        transcript.activeDocument) {
                        setMiningProposals(
                            "Spurious speaker-label cleanup",
                            jcut::mineSpuriousSpeakerAssignments(
                                transcript.activeDocument->root()));
                    }
                    if (!cache.miningProposalLabel.empty()) {
                        ImGui::Text(
                            "%s: %zu proposal%s",
                            cache.miningProposalLabel.c_str(),
                            cache.miningProposals.size(),
                            cache.miningProposals.size() == 1 ? "" : "s");
                    }
                    if (!cache.miningProposals.empty() &&
                        ImGui::BeginTable(
                            "TranscriptMiningProposals",
                            6,
                            ImGuiTableFlags_Borders |
                                ImGuiTableFlags_RowBg |
                                ImGuiTableFlags_Resizable |
                                ImGuiTableFlags_ScrollY,
                            ImVec2(0.0f, 180.0f))) {
                        ImGui::TableSetupColumn(
                            "Apply", ImGuiTableColumnFlags_WidthFixed, 42.0f);
                        ImGui::TableSetupColumn("Target");
                        ImGui::TableSetupColumn("Field");
                        ImGui::TableSetupColumn("Current");
                        ImGui::TableSetupColumn("Proposed");
                        ImGui::TableSetupColumn(
                            "Confidence",
                            ImGuiTableColumnFlags_WidthFixed, 75.0f);
                        ImGui::TableHeadersRow();
                        for (std::size_t index = 0;
                             index < cache.miningProposals.size(); ++index) {
                            const auto& proposal =
                                cache.miningProposals[index];
                            ImGui::PushID(static_cast<int>(index));
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            bool selected =
                                cache.miningProposalSelected[index] != 0;
                            if (ImGui::Checkbox("##apply", &selected)) {
                                cache.miningProposalSelected[index] =
                                    selected ? 1 : 0;
                            }
                            ImGui::TableNextColumn();
                            ImGui::TextUnformatted(proposal.targetId.c_str());
                            ImGui::TableNextColumn();
                            const char* field =
                                proposal.field ==
                                    jcut::TranscriptMiningField::SpeakerName
                                ? "Name"
                                : (proposal.field ==
                                       jcut::TranscriptMiningField::
                                           SpeakerOrganization
                                    ? "Organization" : "Speaker");
                            ImGui::TextUnformatted(field);
                            ImGui::TableNextColumn();
                            ImGui::TextUnformatted(
                                proposal.currentValue.c_str());
                            ImGui::TableNextColumn();
                            ImGui::TextUnformatted(
                                proposal.proposedValue.c_str());
                            if (ImGui::IsItemHovered()) {
                                ImGui::SetTooltip(
                                    "%s", proposal.rationale.c_str());
                            }
                            ImGui::TableNextColumn();
                            ImGui::Text(
                                "%.0f%%", proposal.confidence * 100.0);
                            ImGui::PopID();
                        }
                        ImGui::EndTable();
                    }
                    const bool haveSelectedMiningProposal =
                        std::any_of(
                            cache.miningProposalSelected.begin(),
                            cache.miningProposalSelected.end(),
                            [](std::uint8_t selected) {
                                return selected != 0;
                            });
                    ImGui::BeginDisabled(
                        !transcript.activeCutMutable ||
                        !haveSelectedMiningProposal);
                    if (ImGui::Button("Apply Selected Mining Proposals") &&
                        transcript.activeDocument) {
                        std::vector<jcut::TranscriptMiningProposal> selected;
                        for (std::size_t index = 0;
                             index < cache.miningProposals.size(); ++index) {
                            if (cache.miningProposalSelected[index]) {
                                selected.push_back(
                                    cache.miningProposals[index]);
                            }
                        }
                        nlohmann::json root =
                            transcript.activeDocument->root();
                        std::string error;
                        if (jcut::applyTranscriptMiningProposals(
                                &root, selected, &error)) {
                            saveTranscriptMutation(
                                shellState, &cache, std::move(root));
                            cache.miningProposals.clear();
                            cache.miningProposalSelected.clear();
                            cache.miningProposalLabel.clear();
                        } else if (!error.empty()) {
                            cache.mutationError = std::move(error);
                        }
                    }
                    ImGui::EndDisabled();

                    ImGui::SeparatorText("Face Detection Job");
                    const jcut::FaceProcessingJobSnapshot faceJob =
                        shellState->faceProcessingJob.snapshot();
                    const int faceJobState = static_cast<int>(faceJob.state);
                    if (cache.faceJobLastState != faceJobState) {
                        cache.faceJobLastState = faceJobState;
                        if (faceJob.state ==
                            jcut::FaceProcessingJobSnapshot::State::Completed) {
                            shellState->statusMessage = faceJob.status;
                            const std::string completedClipIdentity =
                                currentClip->persistentId.empty()
                                    ? std::to_string(currentClip->id)
                                    : currentClip->persistentId;
                            cache.faceInspection = jcut::inspectFaceArtifacts(
                                transcript.activePath, completedClipIdentity);
                            cache.selectedFaceTrackIds.clear();
                        } else if (faceJob.state ==
                                   jcut::FaceProcessingJobSnapshot::State::Failed ||
                                   faceJob.state ==
                                   jcut::FaceProcessingJobSnapshot::State::Paused) {
                            shellState->statusMessage = faceJob.status;
                        }
                    }
                    if (faceJob.state !=
                        jcut::FaceProcessingJobSnapshot::State::Idle) {
                        ImGui::TextWrapped("%s", faceJob.status.c_str());
                        if (!faceJob.outputDirectory.empty()) {
                            ImGui::TextDisabled(
                                "Artifacts: %s", faceJob.outputDirectory.c_str());
                        }
                        if (!faceJob.logPath.empty()) {
                            ImGui::TextDisabled("Log: %s", faceJob.logPath.c_str());
                        }
                    } else {
                        ImGui::TextDisabled(
                            "Runs the shared offscreen SCRFD/Vulkan generator and writes "
                            "the same resumable sidecar artifacts used by Qt.");
                    }
                    ImGui::BeginDisabled(faceJob.active());
                    ImGui::SetNextItemWidth(110.0f);
                    ImGui::InputInt("Stride", &cache.faceJobStride);
                    cache.faceJobStride = std::clamp(cache.faceJobStride, 1, 120);
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(110.0f);
                    ImGui::InputDouble(
                        "Threshold", &cache.faceJobThreshold, 0.01, 0.1, "%.3f");
                    cache.faceJobThreshold =
                        std::clamp(cache.faceJobThreshold, 0.0, 1.0);
                    ImGui::SetNextItemWidth(110.0f);
                    ImGui::InputInt("Workers", &cache.faceJobWorkers);
                    cache.faceJobWorkers =
                        std::clamp(cache.faceJobWorkers, 1, 10);
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(110.0f);
                    ImGui::InputInt(
                        "Pipeline Slots", &cache.faceJobPipelineSlots);
                    cache.faceJobPipelineSlots =
                        std::clamp(cache.faceJobPipelineSlots, 1, 10);
                    ImGui::Checkbox(
                        "Primary face only", &cache.faceJobPrimaryOnly);
                    ImGui::SameLine();
                    ImGui::Checkbox(
                        "Small-face fallback", &cache.faceJobSmallFaceFallback);
                    ImGui::Checkbox("SCRFD tiling", &cache.faceJobTiling);
                    ImGui::SameLine();
                    ImGui::Checkbox(
                        "Allow CPU upload compatibility",
                        &cache.faceJobAllowCpuFallback);
                    if (ImGui::Button("Generate Detection + Continuity")) {
                        const fs::path mediaPath =
                            resolvedClipMediaPathForProbe(*shellState, *currentClip);
                        const std::string clipIdentity =
                            currentClip->persistentId.empty()
                                ? std::to_string(currentClip->id)
                                : currentClip->persistentId;
                        jcut::FaceProcessingJobRequest request;
                        request.executablePath =
                            pathString(executableDirPath() /
                                       "jcut_vulkan_facedetections_offscreen");
                        request.mediaPath = pathString(mediaPath);
                        request.transcriptPath = transcript.activePath;
                        request.clipId = clipIdentity;
                        request.outputDirectory =
                            jcut::faceProcessingSidecarDirectory(
                                request.mediaPath, request.clipId);
                        request.detectorSettingsPath =
                            pathString(mediaPath.parent_path() /
                                (mediaPath.stem().string() +
                                 "_detectorsettings.json"));
                        if (!fs::is_regular_file(request.detectorSettingsPath)) {
                            request.detectorSettingsPath.clear();
                        }
                        request.startFrame =
                            std::max<std::int64_t>(0, currentClip->sourceInFrame);
                        request.maxFrames = currentClip->sourceDurationFrames > 0
                            ? currentClip->sourceDurationFrames
                            : std::max(0, currentClip->durationFrames);
                        request.stride = cache.faceJobStride;
                        request.detectorWorkers = cache.faceJobWorkers;
                        request.detectorPipelineSlots =
                            cache.faceJobPipelineSlots;
                        request.threshold = cache.faceJobThreshold;
                        request.primaryFaceOnly = cache.faceJobPrimaryOnly;
                        request.smallFaceFallback =
                            cache.faceJobSmallFaceFallback;
                        request.scrfdTiling = cache.faceJobTiling;
                        request.allowCpuUploadFallback =
                            cache.faceJobAllowCpuFallback;
                        std::string error;
                        if (!shellState->faceProcessingJob.start(request, &error)) {
                            shellState->statusMessage = error.empty()
                                ? "could not start face detection job" : error;
                        } else {
                            shellState->statusMessage =
                                "face detection and continuity generation started";
                        }
                    }
                    ImGui::EndDisabled();
                    if (faceJob.active()) {
                        ImGui::SameLine();
                        if (ImGui::Button("Cancel Face Job")) {
                            shellState->faceProcessingJob.cancel();
                        }
                    }

                    ImGui::SeparatorText("Continuity Tracks");
                    const std::string clipIdentity = currentClip->persistentId.empty()
                        ? std::to_string(currentClip->id)
                        : currentClip->persistentId;
                    const std::string artifactContext =
                        transcript.activePath + "::" + clipIdentity;
                    if (cache.faceArtifactContext != artifactContext) {
                        cache.faceArtifactContext = artifactContext;
                        cache.faceInspection = jcut::inspectFaceArtifacts(
                            transcript.activePath, clipIdentity);
                        cache.selectedFaceTrackIds.clear();
                    }
                    if (ImGui::SmallButton("Refresh Face Artifacts")) {
                        cache.faceInspection = jcut::inspectFaceArtifacts(
                            transcript.activePath, clipIdentity);
                    }
                    const auto assignments =
                        jcut::transcriptSpeakerTrackAssignments(
                            transcript.activeDocument->root(), clipIdentity);
                    const auto assignedIdentity = [&](int trackId) -> std::string {
                        const auto found = std::find_if(
                            assignments.begin(), assignments.end(),
                            [&](const auto& assignment) {
                                return assignment.trackId == trackId;
                            });
                        return found == assignments.end()
                            ? std::string{} : found->identityId;
                    };
                    if (!cache.faceInspection.error.empty()) {
                        ImGui::TextDisabled("%s", cache.faceInspection.error.c_str());
                    } else {
                        ImGui::Text(
                            "%zu tracks | detector: %s | frames: %lld | identity: %zu/%zu",
                            cache.faceInspection.tracks.size(),
                            cache.faceInspection.detectorMode.empty()
                                ? "unknown" : cache.faceInspection.detectorMode.c_str(),
                            static_cast<long long>(cache.faceInspection.rawFrameCount),
                            cache.faceInspection.identityAssignmentCount,
                            cache.faceInspection.identityClusterCount);
                        if (!cache.faceInspection.warning.empty()) {
                            ImGui::TextColored(
                                ImVec4(0.95f, 0.72f, 0.28f, 1.0f),
                                "%s", cache.faceInspection.warning.c_str());
                        }
                        if (ImGui::BeginTable(
                                "ContinuityTracks",
                                7,
                                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                    ImGuiTableFlags_Resizable |
                                    ImGuiTableFlags_ScrollY,
                                ImVec2(0.0f, 210.0f))) {
                            ImGui::TableSetupColumn(
                                "Use", ImGuiTableColumnFlags_WidthFixed, 38.0f);
                            ImGui::TableSetupColumn(
                                "Track", ImGuiTableColumnFlags_WidthFixed, 52.0f);
                            ImGui::TableSetupColumn(
                                "Samples", ImGuiTableColumnFlags_WidthFixed, 62.0f);
                            ImGui::TableSetupColumn("Frames");
                            ImGui::TableSetupColumn("Position");
                            ImGui::TableSetupColumn(
                                "Score", ImGuiTableColumnFlags_WidthFixed, 55.0f);
                            ImGui::TableSetupColumn("Assigned");
                            ImGui::TableHeadersRow();
                            for (const auto& track : cache.faceInspection.tracks) {
                                ImGui::PushID(track.trackId);
                                ImGui::TableNextRow();
                                ImGui::TableNextColumn();
                                auto selected = std::find(
                                    cache.selectedFaceTrackIds.begin(),
                                    cache.selectedFaceTrackIds.end(),
                                    track.trackId);
                                bool checked =
                                    selected != cache.selectedFaceTrackIds.end();
                                if (ImGui::Checkbox("##track", &checked)) {
                                    if (checked && selected == cache.selectedFaceTrackIds.end()) {
                                        cache.selectedFaceTrackIds.push_back(track.trackId);
                                    } else if (!checked &&
                                               selected != cache.selectedFaceTrackIds.end()) {
                                        cache.selectedFaceTrackIds.erase(selected);
                                    }
                                }
                                ImGui::TableNextColumn();
                                ImGui::Text("%d", track.trackId);
                                ImGui::TableNextColumn();
                                ImGui::Text("%zu", track.sampleCount);
                                ImGui::TableNextColumn();
                                ImGui::Text("%lld-%lld",
                                    static_cast<long long>(track.firstFrame),
                                    static_cast<long long>(track.lastFrame));
                                ImGui::TableNextColumn();
                                ImGui::Text("%.2f, %.2f / %.2f",
                                    track.x, track.y, track.box);
                                ImGui::TableNextColumn();
                                ImGui::Text("%.2f", track.score);
                                ImGui::TableNextColumn();
                                const std::string identity =
                                    assignedIdentity(track.trackId);
                                ImGui::TextUnformatted(
                                    identity.empty() ? "Unassigned" : identity.c_str());
                                ImGui::PopID();
                            }
                            ImGui::EndTable();
                        }
                    }
                    ImGui::BeginDisabled(
                        !transcript.activeCutMutable ||
                        cache.selectedSpeakerId.empty() ||
                        cache.selectedFaceTrackIds.empty());
                    if (ImGui::Button("Assign Selected Tracks") &&
                        transcript.activeDocument) {
                        std::vector<jcut::TranscriptTrackAssignmentAnchor> anchors;
                        for (const auto& track : cache.faceInspection.tracks) {
                            if (std::find(
                                    cache.selectedFaceTrackIds.begin(),
                                    cache.selectedFaceTrackIds.end(),
                                    track.trackId) ==
                                cache.selectedFaceTrackIds.end()) {
                                continue;
                            }
                            anchors.push_back({
                                track.trackId,
                                track.streamId,
                                std::max<std::int64_t>(0, track.firstFrame),
                                track.x,
                                track.y,
                                track.box});
                        }
                        nlohmann::json root = transcript.activeDocument->root();
                        std::string error;
                        if (jcut::setTranscriptSpeakerTrackAssignments(
                                &root,
                                clipIdentity,
                                cache.selectedSpeakerId,
                                anchors,
                                false,
                                {},
                                &error)) {
                            saveTranscriptMutation(
                                shellState, &cache, std::move(root));
                        } else if (!error.empty()) {
                            cache.mutationError = std::move(error);
                        }
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::BeginDisabled(
                        !transcript.activeCutMutable ||
                        cache.selectedSpeakerId.empty());
                    if (ImGui::Button("Clear Speaker Tracks") &&
                        transcript.activeDocument) {
                        nlohmann::json root = transcript.activeDocument->root();
                        std::string error;
                        if (jcut::setTranscriptSpeakerTrackAssignments(
                                &root,
                                clipIdentity,
                                cache.selectedSpeakerId,
                                {},
                                true,
                                {},
                                &error)) {
                            saveTranscriptMutation(
                                shellState, &cache, std::move(root));
                        } else if (!error.empty()) {
                            cache.mutationError = std::move(error);
                        }
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::BeginDisabled(cache.selectedFaceTrackIds.size() != 1);
                    if (ImGui::Button("View Track Reference")) {
                        const int selectedTrackId =
                            cache.selectedFaceTrackIds.front();
                        const auto selectedTrack = std::find_if(
                            cache.faceInspection.tracks.begin(),
                            cache.faceInspection.tracks.end(),
                            [&](const auto& track) {
                                return track.trackId == selectedTrackId;
                            });
                        if (selectedTrack != cache.faceInspection.tracks.end()) {
                            const std::int64_t timelineFrame =
                                jcut::faceTrackAnchorTimelineFrame(
                                    selectedTrack->firstFrame,
                                    currentClip->sourceInFrame,
                                    currentClip->startFrame,
                                    currentClip->durationFrames,
                                    currentClip->playbackRate);
                            applyCommand(
                                shellState,
                                jcut::SeekToFrameCommand{
                                    static_cast<int>(timelineFrame)});
                            shellState->statusMessage =
                                "showing selected face-track reference frame";
                        }
                    }
                    ImGui::EndDisabled();
                    ImGui::TextDisabled(
                        "The selected reference track is outlined in the Program monitor. "
                        "Cloud enrichment and cropped reference galleries remain pending.");
                }
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
            bool canUndo = false;
            bool canRedo = false;
            std::size_t undoDepth = 0;
            std::size_t redoDepth = 0;
            {
                std::lock_guard<std::mutex> lock(shellState->runtimeMutex);
                canUndo = shellState->runtime.canUndo();
                canRedo = shellState->runtime.canRedo();
                undoDepth = shellState->runtime.undoDepth();
                redoDepth = shellState->runtime.redoDepth();
            }
            ImGui::BeginDisabled(!canUndo);
            if (ImGui::Button("Undo")) {
                applyCommand(shellState, jcut::UndoCommand{});
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!canRedo);
            if (ImGui::Button("Redo")) {
                applyCommand(shellState, jcut::RedoCommand{});
            }
            ImGui::EndDisabled();
            if (ImGui::BeginTable("HistoryTable", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Stack");
                ImGui::TableSetupColumn("Status");
                ImGui::TableHeadersRow();
                drawReadOnlyTableRow("Undo", std::to_string(undoDepth) + " snapshots");
                drawReadOnlyTableRow("Redo", std::to_string(redoDepth) + " snapshots");
                ImGui::EndTable();
            }

            ImGui::Separator();
            ImGui::TextUnformatted("Saved Project History");
            if (!shellState->usesQtProjectStorage) {
                ImGui::TextDisabled(
                    "Saved history navigation is available only for Qt project storage.");
            } else {
                if (ImGui::SmallButton("Refresh Saved History")) {
                    shellState->projectHistoryRefreshRequested = true;
                }
                if (shellState->projectHistoryRefreshRequested) {
                    std::string historyError;
                    const std::optional<
                        std::vector<jcut::ImGuiProjectHistoryEntry>> entries =
                            jcut::listImGuiProjectHistoryEntries(
                                currentProjectSession(*shellState, snapshot),
                                &historyError);
                    shellState->projectHistoryEntries =
                        entries.value_or(
                            std::vector<jcut::ImGuiProjectHistoryEntry>{});
                    shellState->projectHistoryError = std::move(historyError);
                    shellState->projectHistoryRefreshRequested = false;
                }
                if (!shellState->projectHistoryError.empty()) {
                    ImGui::TextWrapped(
                        "%s", shellState->projectHistoryError.c_str());
                } else {
                    const bool savedHistoryBlocked =
                        documentIsDirty(*shellState, snapshot);
                    if (savedHistoryBlocked) {
                        ImGui::TextDisabled(
                            "Save changes before restoring a saved project-history snapshot.");
                    }
                    if (ImGui::BeginTable(
                            "SavedProjectHistoryTable",
                            5,
                            ImGuiTableFlags_Borders |
                                ImGuiTableFlags_RowBg |
                                ImGuiTableFlags_ScrollY,
                            ImVec2(0.0f, 170.0f))) {
                        ImGui::TableSetupScrollFreeze(0, 1);
                        ImGui::TableSetupColumn("Snapshot");
                        ImGui::TableSetupColumn("Project");
                        ImGui::TableSetupColumn("Frame");
                        ImGui::TableSetupColumn("Clips");
                        ImGui::TableSetupColumn("Action");
                        ImGui::TableHeadersRow();
                        for (const jcut::ImGuiProjectHistoryEntry& entry :
                             shellState->projectHistoryEntries) {
                            ImGui::PushID(static_cast<int>(entry.index));
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::Text("%zu", entry.index + 1);
                            ImGui::TableNextColumn();
                            ImGui::TextWrapped(
                                "%s",
                                entry.projectName.empty()
                                    ? "Untitled Project"
                                    : entry.projectName.c_str());
                            ImGui::TableNextColumn();
                            ImGui::Text(
                                "%lld",
                                static_cast<long long>(entry.currentFrame));
                            ImGui::TableNextColumn();
                            ImGui::Text("%zu", entry.clipCount);
                            ImGui::TableNextColumn();
                            if (entry.isActive) {
                                ImGui::TextUnformatted("Active");
                            } else {
                                ImGui::BeginDisabled(savedHistoryBlocked);
                                if (ImGui::SmallButton("Restore") &&
                                    commitExportOutputPathDraft(shellState)) {
                                    jcut::EditorDocumentCore currentDocument;
                                    {
                                        std::lock_guard<std::mutex> lock(
                                            shellState->runtimeMutex);
                                        currentDocument =
                                            shellState->runtime.snapshot();
                                    }
                                    if (documentIsDirty(
                                            *shellState,
                                            currentDocument)) {
                                        shellState->statusMessage =
                                            "save changes before restoring project history";
                                    } else {
                                        std::string activationError;
                                        const std::optional<
                                            jcut::ImGuiProjectSession> restored =
                                                jcut::activateImGuiProjectHistoryEntry(
                                                    currentProjectSession(
                                                        *shellState,
                                                        currentDocument),
                                                    entry.index,
                                                    &activationError);
                                        if (restored.has_value()) {
                                            loadProjectSessionIntoShell(
                                                shellState,
                                                *restored,
                                                "project history snapshot restored");
                                        } else {
                                            shellState->statusMessage =
                                                activationError;
                                        }
                                    }
                                }
                                ImGui::EndDisabled();
                            }
                            ImGui::PopID();
                        }
                        ImGui::EndTable();
                    }
                }
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
            const jcut::ImGuiAudioStatus audioStatus = shellState->audioRuntime.status();
            bool waveform = snapshot.panels.showWaveform;
            if (ImGui::Checkbox("Waveform", &waveform)) {
                applyCommand(shellState, jcut::SetWaveformVisibleCommand{waveform});
            }
            bool clipAudioEnabled = currentClip ? currentClip->audioEnabled : false;
            float clipGain = currentClip ? static_cast<float>(currentClip->audioGain) : 1.0f;
            float clipPan = currentClip ? static_cast<float>(currentClip->audioPan) : 0.0f;
            bool clipSolo = currentClip ? currentClip->audioSolo : false;
            ImGui::BeginDisabled(!currentClip);
            bool audioChanged = false;
            audioChanged |= ImGui::Checkbox("Clip Audio", &clipAudioEnabled);
            audioChanged |= ImGui::SliderFloat("Clip Gain", &clipGain, 0.0f, 4.0f, "%.2f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            audioChanged |= ImGui::SliderFloat("Clip Pan", &clipPan, -1.0f, 1.0f, "%.2f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            audioChanged |= ImGui::Checkbox("Clip Solo", &clipSolo);
            if (audioChanged && currentClip) {
                applyCommand(shellState, jcut::SetClipAudioCommand{
                    currentClip->id, clipAudioEnabled, clipGain, clipPan, clipSolo});
            }
            ImGui::EndDisabled();
            if (ImGui::BeginTable("AudioStatus", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                drawReadOnlyTableRow("Initialized", audioStatus.initialized ? "yes" : "no");
                drawReadOnlyTableRow("Timeline", audioStatus.timelineConfigured ? "configured" : "pending");
                drawReadOnlyTableRow("Buffering", audioStatus.buffering ? "yes" : "no");
                drawReadOnlyTableRow("Playback", audioStatus.playbackActive ? "active" : "idle");
                drawReadOnlyTableRow("Sources", std::to_string(audioStatus.scheduledSourcePaths.size()));
                if (!audioStatus.scheduledSourcePaths.empty()) {
                    drawReadOnlyTableRow("Active Source", audioStatus.scheduledSourcePaths.front());
                }
                drawReadOnlyTableRow("Status", audioStatus.message);
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Jobs")) {
            drawInspectorHeading("Processing Jobs", snapshot, currentClip);
            bool exportActive = false;
            bool exportQueued = false;
            jcut::render::RenderProgressCore progress;
            jcut::render::RenderResultCore result;
            {
                std::lock_guard<std::mutex> lock(shellState->exportMutex);
                exportActive = shellState->exportRunning;
                exportQueued = shellState->exportRequested;
                progress = shellState->exportProgress;
                result = shellState->exportResult;
            }
            const jcut::FaceProcessingJobSnapshot faceJob =
                shellState->faceProcessingJob.snapshot();
            const jcut::ProxyGenerationJobSnapshot proxyJob =
                shellState->proxyGenerationJob.snapshot();
            const auto faceStateLabel = [](jcut::FaceProcessingJobSnapshot::State state) {
                using State = jcut::FaceProcessingJobSnapshot::State;
                switch (state) {
                case State::Idle: return "idle";
                case State::Starting: return "starting";
                case State::Running: return "running";
                case State::Canceling: return "canceling";
                case State::Paused: return "paused";
                case State::Completed: return "complete";
                case State::Failed: return "failed";
                }
                return "unknown";
            };
            const auto proxyStateLabel = [](jcut::ProxyGenerationJobSnapshot::State state) {
                using State = jcut::ProxyGenerationJobSnapshot::State;
                switch (state) {
                case State::Idle: return "idle";
                case State::Starting: return "starting";
                case State::Running: return "running";
                case State::Canceling: return "canceling";
                case State::Completed: return "complete";
                case State::Canceled: return "canceled";
                case State::Failed: return "failed";
                }
                return "unknown";
            };
            if (ImGui::BeginTable("JobsTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Job");
                ImGui::TableSetupColumn("Status");
                ImGui::TableSetupColumn("Progress");
                ImGui::TableHeadersRow();
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted("Timeline export");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(exportActive ? "running" :
                                       (exportQueued ? "queued" :
                                        (result.message.empty() ? "idle" :
                                         (result.success ? "complete" : "failed"))));
                ImGui::TableNextColumn();
                ImGui::Text("%lld / %lld",
                            static_cast<long long>(progress.framesCompleted),
                            static_cast<long long>(progress.totalFrames));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted("Face detection");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(faceStateLabel(faceJob.state));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(faceJob.active()
                    ? "external worker"
                    : (faceJob.exitCode >= 0
                        ? ("exit " + std::to_string(faceJob.exitCode)).c_str()
                        : "-"));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted("Proxy generation");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(proxyStateLabel(proxyJob.state));
                ImGui::TableNextColumn();
                ImGui::Text(
                    "%lld / %lld",
                    static_cast<long long>(proxyJob.framesCompleted),
                    static_cast<long long>(proxyJob.totalFrames));
                ImGui::EndTable();
            }
            ImGui::BeginDisabled(!exportActive && !exportQueued);
            if (ImGui::Button("Cancel Export")) {
                cancelExportRender(shellState);
                shellState->statusMessage = "export cancellation requested";
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!proxyJob.active());
            if (ImGui::Button("Cancel Proxy Generation")) {
                shellState->proxyGenerationJob.cancel();
                shellState->statusMessage =
                    "proxy-generation cancellation requested";
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!faceJob.active());
            if (ImGui::Button("Cancel Face Detection")) {
                shellState->faceProcessingJob.cancel();
                shellState->statusMessage =
                    "face-detection cancellation requested";
            }
            ImGui::EndDisabled();
            if (!result.message.empty()) {
                ImGui::TextWrapped("%s", result.message.c_str());
            }
            if (!faceJob.status.empty()) {
                ImGui::TextWrapped("Face detection: %s", faceJob.status.c_str());
            }
            if (!faceJob.outputDirectory.empty()) {
                ImGui::TextWrapped(
                    "Output: %s", faceJob.outputDirectory.c_str());
            }
            if (!faceJob.manifestPath.empty()) {
                ImGui::TextWrapped(
                    "Manifest: %s", faceJob.manifestPath.c_str());
            }
            if (!faceJob.logPath.empty()) {
                ImGui::TextWrapped("Log: %s", faceJob.logPath.c_str());
            }
            if (!proxyJob.status.empty()) {
                ImGui::TextWrapped(
                    "Proxy generation: %s", proxyJob.status.c_str());
            }
            if (!proxyJob.outputDirectory.empty()) {
                ImGui::TextWrapped(
                    "Proxy output: %s",
                    proxyJob.outputDirectory.c_str());
            }
            if (!proxyJob.manifestPath.empty()) {
                ImGui::TextWrapped(
                    "Proxy manifest: %s",
                    proxyJob.manifestPath.c_str());
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("AI Assist")) {
            drawInspectorHeading("AI Assist", snapshot, currentClip);
            ImGui::TextWrapped(
                "Reviewed deterministic speaker-name, organization, and "
                "spurious-label mining is available in Speakers. Cloud/model "
                "enrichment and chat remain pending extraction from the Qt adapter.");
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
            ImGui::TextDisabled("Entitlement refresh requires the shared access-service adapter.");
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
                const bool trackChanged = ImGui::InputInt("Track", &trackId);
                beginRuntimeHistoryTransactionForLastItem(shellState);
                if (trackChanged) {
                    applyCommand(shellState, jcut::MoveClipCommand{
                        selectedClip->id, trackId, selectedClip->startFrame});
                }

                int startFrame = selectedClip->startFrame;
                const bool startChanged = ImGui::InputInt("Start", &startFrame);
                beginRuntimeHistoryTransactionForLastItem(shellState);
                if (startChanged) {
                    applyCommand(shellState, jcut::MoveClipCommand{
                        selectedClip->id, selectedClip->trackId, startFrame});
                }

                int durationFrames = selectedClip->durationFrames;
                const bool durationChanged = ImGui::InputInt("Duration", &durationFrames);
                beginRuntimeHistoryTransactionForLastItem(shellState);
                if (durationChanged) {
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
                ImGui::SeparatorText("Proxy");
                if (shellState->proxyPathDraftClipId != selectedClip->id) {
                    shellState->proxyPathDraftClipId = selectedClip->id;
                    shellState->proxyPathDraft = selectedClip->proxyPath;
                }
                ImGui::InputText(
                    "Proxy Path", &shellState->proxyPathDraft);
                const bool configuredProxyUsable =
                    jcut::proxyPathIsUsable(selectedClip->proxyPath);
                ImGui::Text(
                    "Configured: %s | Playback: %s",
                    selectedClip->proxyPath.empty()
                        ? "none"
                        : (configuredProxyUsable ? "ready" : "missing"),
                    selectedClip->useProxy && configuredProxyUsable
                        ? "proxy" : "source");
                if (ImGui::Button("Attach Proxy")) {
                    fs::path proxyPath(shellState->proxyPathDraft);
                    if (proxyPath.is_relative()) {
                        const fs::path root =
                            !shellState->mediaRootDirectory.empty()
                            ? fs::path(shellState->mediaRootDirectory)
                            : fs::path(shellState->projectRootPath);
                        proxyPath = root / proxyPath;
                    }
                    proxyPath = proxyPath.lexically_normal();
                    if (!jcut::proxyPathIsUsable(pathString(proxyPath))) {
                        shellState->statusMessage =
                            "proxy path is not a readable media file or image sequence";
                    } else {
                        shellState->proxyPathDraft = pathString(proxyPath);
                        applyCommand(
                            shellState,
                            jcut::SetClipProxyCommand{
                                selectedClip->id,
                                shellState->proxyPathDraft,
                                true});
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Discover Proxy")) {
                    const fs::path sourcePath =
                        resolvedClipMediaPathForProbe(
                            *shellState, *selectedClip);
                    const std::string discovered =
                        jcut::discoverExistingProxyPath(
                            pathString(sourcePath));
                    if (discovered.empty()) {
                        shellState->statusMessage =
                            "no default .proxy, .proxy.mp4, or .proxy.mov was found";
                    } else {
                        shellState->proxyPathDraft = discovered;
                        applyCommand(
                            shellState,
                            jcut::SetClipProxyCommand{
                                selectedClip->id, discovered, true});
                    }
                }
                const jcut::ProxyGenerationJobSnapshot proxyJob =
                    shellState->proxyGenerationJob.snapshot();
                const fs::path proxySource =
                    resolvedClipMediaPathForProbe(
                        *shellState, *selectedClip);
                const std::array<const char*, 3> proxyFormats{
                    "Image Sequence (JPEG)",
                    "H.264 (MP4)",
                    "Motion JPEG (MOV)"};
                shellState->proxyGenerationFormatIndex = std::clamp(
                    shellState->proxyGenerationFormatIndex, 0, 2);
                ImGui::Combo(
                    "Generation Format",
                    &shellState->proxyGenerationFormatIndex,
                    proxyFormats.data(),
                    static_cast<int>(proxyFormats.size()));
                const jcut::ProxyGenerationFormat proxyFormat =
                    shellState->proxyGenerationFormatIndex == 1
                    ? jcut::ProxyGenerationFormat::H264Mp4
                    : (shellState->proxyGenerationFormatIndex == 2
                        ? jcut::ProxyGenerationFormat::MjpegMov
                        : jcut::ProxyGenerationFormat::ImageSequenceJpeg);
                const std::string generatedProxyPath =
                    jcut::defaultProxyOutputPath(
                        pathString(proxySource), proxyFormat);
                const bool generatedProxyExists =
                    jcut::proxyPathIsUsable(generatedProxyPath);
                ImGui::Checkbox(
                    "Overwrite Existing Image Proxy",
                    &shellState->overwriteProxyGeneration);
                ImGui::BeginDisabled(
                    proxyJob.active() ||
                    generatedProxyPath.empty() ||
                    (generatedProxyExists &&
                     !shellState->overwriteProxyGeneration));
                if (ImGui::Button("Create Proxy")) {
                    std::string error;
                    if (shellState->proxyGenerationJob.start(
                            {selectedClip->id,
                             pathString(proxySource),
                             generatedProxyPath,
                             proxyFormat,
                             false,
                             shellState->overwriteProxyGeneration},
                            &error)) {
                        shellState->statusMessage =
                            "proxy generation started";
                    } else {
                        shellState->statusMessage = error.empty()
                            ? "proxy generation could not start"
                            : error;
                    }
                }
                ImGui::EndDisabled();
                if (generatedProxyExists &&
                    !shellState->overwriteProxyGeneration) {
                    ImGui::TextDisabled(
                        "Enable overwrite to regenerate the existing proxy artifact.");
                }
                ImGui::SameLine();
                ImGui::BeginDisabled(
                    proxyJob.active() ||
                    proxyFormat !=
                        jcut::ProxyGenerationFormat::ImageSequenceJpeg ||
                    !generatedProxyExists);
                if (ImGui::Button("Continue Proxy")) {
                    std::string error;
                    if (shellState->proxyGenerationJob.start(
                            {selectedClip->id,
                             pathString(proxySource),
                             generatedProxyPath,
                             proxyFormat,
                             true,
                             false},
                            &error)) {
                        shellState->statusMessage =
                            "proxy continuation started";
                    } else {
                        shellState->statusMessage = error.empty()
                            ? "proxy continuation could not start"
                            : error;
                    }
                }
                ImGui::EndDisabled();
                if (proxyJob.state ==
                        jcut::ProxyGenerationJobSnapshot::State::Completed &&
                    proxyJob.clipId == selectedClip->id &&
                    jcut::proxyPathIsUsable(proxyJob.outputDirectory)) {
                    ImGui::SameLine();
                    if (ImGui::Button("Attach Generated Proxy")) {
                        shellState->proxyPathDraft =
                            proxyJob.outputDirectory;
                        applyCommand(
                            shellState,
                            jcut::SetClipProxyCommand{
                                selectedClip->id,
                                proxyJob.outputDirectory,
                                true});
                    }
                }
                const std::vector<std::string> allProxyCandidates =
                    jcut::proxyCandidatePaths(pathString(proxySource));
                const bool configuredIsManagedProxy =
                    std::find(
                        allProxyCandidates.begin(),
                        allProxyCandidates.end(),
                        selectedClip->proxyPath) != allProxyCandidates.end();
                const std::string proxyDeletionTarget =
                    configuredIsManagedProxy
                    ? selectedClip->proxyPath
                    : generatedProxyPath;
                ImGui::BeginDisabled(
                    proxyJob.active() ||
                    !jcut::proxyPathIsUsable(proxyDeletionTarget));
                if (ImGui::Button("Delete Proxy File...")) {
                    ImGui::OpenPopup("Confirm Proxy Deletion");
                }
                ImGui::EndDisabled();
                if (ImGui::BeginPopupModal(
                        "Confirm Proxy Deletion",
                        nullptr,
                        ImGuiWindowFlags_AlwaysAutoResize)) {
                    ImGui::TextWrapped(
                        "Permanently delete this generated proxy?\n%s",
                        proxyDeletionTarget.c_str());
                    ImGui::TextDisabled(
                        "This removes the proxy file or image directory. "
                        "The source media is not modified.");
                    if (ImGui::Button("Delete Permanently")) {
                        std::string error;
                        if (jcut::removeProxyArtifact(
                                pathString(proxySource),
                                proxyDeletionTarget,
                                &error)) {
                            if (selectedClip->proxyPath ==
                                proxyDeletionTarget) {
                                shellState->proxyPathDraft.clear();
                                applyCommand(
                                    shellState,
                                    jcut::SetClipProxyCommand{
                                        selectedClip->id, {}, false});
                            }
                            shellState->statusMessage =
                                "proxy artifact permanently deleted";
                            ImGui::CloseCurrentPopup();
                        } else {
                            shellState->statusMessage = error.empty()
                                ? "proxy artifact could not be deleted"
                                : error;
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel")) {
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndPopup();
                }
                ImGui::BeginDisabled(selectedClip->proxyPath.empty());
                bool useProxy = selectedClip->useProxy;
                if (ImGui::Checkbox("Use Proxy for Playback", &useProxy)) {
                    applyCommand(
                        shellState,
                        jcut::SetClipProxyCommand{
                            selectedClip->id,
                            selectedClip->proxyPath,
                            useProxy});
                }
                ImGui::SameLine();
                if (ImGui::Button("Clear Proxy Association")) {
                    shellState->proxyPathDraft.clear();
                    applyCommand(
                        shellState,
                        jcut::SetClipProxyCommand{
                            selectedClip->id, {}, false});
                }
                ImGui::EndDisabled();
                ImGui::TextDisabled(
                    "Association changes do not delete proxy files. "
                    "Proxy encoding remains a separate processing job.");
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
        if (ImGui::BeginTabItem("Output",
                                nullptr,
                                focusOutput ? ImGuiTabItemFlags_SetSelected
                                            : ImGuiTabItemFlags_None)) {
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
            const std::array<const char*, 4> formats = {"mp4", "mov", "mkv", "webm"};
            int formatIndex = 0;
            for (int i = 0; i < static_cast<int>(formats.size()); ++i) {
                if (snapshot.exportRequest.outputFormat == formats[i]) {
                    formatIndex = i;
                    break;
                }
            }
            const bool widthChanged = ImGui::InputInt("Width", &width);
            beginRuntimeHistoryTransactionForLastItem(shellState);
            if (widthChanged) {
                applyCommand(shellState, jcut::SetExportSizeCommand{width, height});
            }
            const bool heightChanged = ImGui::InputInt("Height", &height);
            beginRuntimeHistoryTransactionForLastItem(shellState);
            if (heightChanged) {
                applyCommand(shellState, jcut::SetExportSizeCommand{width, height});
            }
            const bool fpsChanged = ImGui::InputFloat("FPS", &fps, 0.5f, 2.0f, "%.2f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            if (fpsChanged) {
                applyCommand(shellState, jcut::SetExportFpsCommand{fps});
            }
            int exportStart = static_cast<int>(snapshot.exportRequest.exportStartFrame);
            int exportEnd = static_cast<int>(snapshot.exportRequest.exportEndFrame);
            bool exportRangeChanged = false;
            exportRangeChanged |= ImGui::InputInt("Export Start", &exportStart);
            beginRuntimeHistoryTransactionForLastItem(shellState);
            exportRangeChanged |= ImGui::InputInt("Export End", &exportEnd);
            beginRuntimeHistoryTransactionForLastItem(shellState);
            if (exportRangeChanged) {
                applyCommand(shellState, jcut::SetExportRangeCommand{exportStart, exportEnd});
            }
            ImGui::InputText("Output Path",
                             shellState->exportOutputPath.data(),
                             shellState->exportOutputPath.size());
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                commitExportOutputPathDraft(shellState);
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
            const PreviewHistogram histogram = currentPreviewHistogram(shellState);
            if (histogram.valid) {
                ImGui::PlotHistogram("Luma",
                                     histogram.luma.data(),
                                     static_cast<int>(histogram.luma.size()),
                                     0, nullptr, 0.0f, 1.0f, ImVec2(-1.0f, 90.0f));
                ImGui::PlotLines("Red",
                                 histogram.red.data(),
                                 static_cast<int>(histogram.red.size()),
                                 0, nullptr, 0.0f, 1.0f, ImVec2(-1.0f, 54.0f));
                ImGui::PlotLines("Green",
                                 histogram.green.data(),
                                 static_cast<int>(histogram.green.size()),
                                 0, nullptr, 0.0f, 1.0f, ImVec2(-1.0f, 54.0f));
                ImGui::PlotLines("Blue",
                                 histogram.blue.data(),
                                 static_cast<int>(histogram.blue.size()),
                                 0, nullptr, 0.0f, 1.0f, ImVec2(-1.0f, 54.0f));
            } else {
                ImGui::TextWrapped("Scopes are unavailable while preview is using a zero-copy GPU frame without CPU readback.");
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Pipeline")) {
            drawInspectorHeading("Pipeline", snapshot, currentClip);
            jcut::standalone_render::PreviewRenderResult previewResult;
            bool lastUsedZeroCopy = false;
            bool zeroCopyAvailable = false;
            std::string zeroCopyFailure;
            {
                std::lock_guard<std::mutex> lock(shellState->previewMutex);
                previewResult = shellState->previewResult;
                lastUsedZeroCopy = shellState->previewLastUsedZeroCopy;
                zeroCopyAvailable = shellState->previewZeroCopyAvailable;
                zeroCopyFailure = shellState->previewZeroCopyFailureReason;
            }
            if (ImGui::BeginTable("PipelineStages", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                drawReadOnlyTableRow("Preview", previewResult.success ? "ready" : "not ready");
                drawReadOnlyTableRow("Render", previewResult.message);
                drawReadOnlyTableRow("Source", previewResult.sourcePath);
                drawReadOnlyTableRow("GPU Frame", previewResult.vulkanFrame.valid ? "valid" : "none");
                drawReadOnlyTableRow("CPU Frame", previewResult.image.empty() ? "none" : "valid");
                drawReadOnlyTableRow("Zero Copy", lastUsedZeroCopy ? "active" : "inactive");
                drawReadOnlyTableRow("Zero Copy Available", zeroCopyAvailable ? "yes" : "no");
                drawReadOnlyTableRow("Fallback", zeroCopyFailure);
                drawReadOnlyTableRow("Present", "raw X11 Vulkan swapchain");
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("System")) {
            drawInspectorHeading("System", snapshot, currentClip);
            const jcut::ImGuiAudioStatus audioStatus = shellState->audioRuntime.status();
            if (ImGui::BeginTable("SystemProfile", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                drawReadOnlyTableRow("Backend", "imgui");
                drawReadOnlyTableRow("Window", "x11/vulkan");
                drawReadOnlyTableRow("Media", std::to_string(snapshot.mediaItems.size()));
                drawReadOnlyTableRow("Clips", std::to_string(snapshot.clips.size()));
                drawReadOnlyTableRow("Tracks", std::to_string(snapshot.tracks.size()));
                drawReadOnlyTableRow("Audio", audioStatus.message);
                drawReadOnlyTableRow(
                    "Audio Buffer Requested",
                    std::to_string(audioStatus.requestedBufferFrames));
                drawReadOnlyTableRow(
                    "Audio Buffer Actual",
                    audioStatus.actualBufferFrames > 0
                    ? std::to_string(audioStatus.actualBufferFrames)
                    : std::string("stream closed"));
                ImGui::EndTable();
            }
            ImGui::TextDisabled("Decoder mutation and benchmark controls require the neutral decoder-control service.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Projects",
                                nullptr,
                                focusProjects ? ImGuiTabItemFlags_SetSelected
                                              : ImGuiTabItemFlags_None)) {
            drawInspectorHeading("Projects", snapshot, currentClip);
            ImGui::TextWrapped("%s", shellState->projectId.empty()
                ? snapshot.projectName.c_str()
                : shellState->projectId.c_str());
            ImGui::TextWrapped("%s", shellState->statePath.empty()
                ? shellState->documentPath.c_str()
                : shellState->statePath.c_str());
            std::string projectListError;
            const std::vector<std::string> projectIds = shellState->usesQtProjectStorage
                ? jcut::availableImGuiProjectIds(&projectListError)
                : std::vector<std::string>{};
            if (!projectListError.empty()) {
                ImGui::TextWrapped("%s", projectListError.c_str());
            }
            if (!shellState->usesQtProjectStorage) {
                ImGui::TextDisabled("Project switching is available only for Qt project storage.");
            }
            ImGui::BeginDisabled(!shellState->usesQtProjectStorage);
            if (ImGui::BeginChild("ProjectList", ImVec2(-1.0f, 150.0f), true)) {
                for (const std::string& projectId : projectIds) {
                    const bool selected = projectId == shellState->projectId;
                    if (ImGui::Selectable(projectId.c_str(), selected) && !selected) {
                        if (!commitExportOutputPathDraft(shellState)) {
                            continue;
                        }
                        jcut::EditorDocumentCore currentDocument;
                        {
                            std::lock_guard<std::mutex> lock(shellState->runtimeMutex);
                            currentDocument = shellState->runtime.snapshot();
                        }
                        if (documentIsDirty(*shellState, currentDocument)) {
                            shellState->statusMessage = "save changes before switching projects";
                        } else {
                            std::string error;
                            const std::optional<jcut::ImGuiProjectSession> session =
                                jcut::activateImGuiProjectSession(projectId, &error);
                            if (session.has_value()) {
                                loadProjectSessionIntoShell(
                                    shellState,
                                    *session,
                                    "active project switched: " + projectId);
                            } else {
                                shellState->statusMessage = error;
                            }
                        }
                    }
                }
            }
            ImGui::EndChild();
            ImGui::EndDisabled();
            if (ImGui::Button("Save")) {
                saveCurrentDocument(shellState);
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(!shellState->usesQtProjectStorage);
            if (ImGui::Button("Save As")) {
                requestProjectLifecycleAction(
                    shellState, ProjectLifecycleAction::SaveAs, snapshot);
            }
            ImGui::EndDisabled();
            const bool projectDocumentDirty = documentIsDirty(*shellState, snapshot);
            ImGui::BeginDisabled(
                !shellState->usesQtProjectStorage || projectDocumentDirty);
            if (ImGui::Button("New")) {
                requestProjectLifecycleAction(
                    shellState, ProjectLifecycleAction::NewProject, snapshot);
            }
            ImGui::SameLine();
            if (ImGui::Button("Rename")) {
                requestProjectLifecycleAction(
                    shellState, ProjectLifecycleAction::Rename, snapshot);
            }
            ImGui::EndDisabled();
            if (projectDocumentDirty && shellState->usesQtProjectStorage) {
                ImGui::TextDisabled(
                    "Save changes before New, Rename, or project switching.");
            }
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
            const std::array<int, 6> audioBufferChoices{
                128, 256, 512, 1024, 2048, 4096};
            const std::array<const char*, 6> audioBufferLabels{
                "128", "256", "512", "1024", "2048", "4096"};
            int audioBufferIndex = 3;
            for (int index = 0;
                 index < static_cast<int>(audioBufferChoices.size());
                 ++index) {
                if (audioBufferChoices[index] ==
                    shellState->audioBufferFrames) {
                    audioBufferIndex = index;
                    break;
                }
            }
            if (ImGui::Combo(
                    "Audio Buffer Frames",
                    &audioBufferIndex,
                    audioBufferLabels.data(),
                    static_cast<int>(audioBufferLabels.size()))) {
                shellState->audioBufferFrames =
                    audioBufferChoices[audioBufferIndex];
                shellState->audioRuntime.setBufferFrames(
                    static_cast<unsigned int>(
                        shellState->audioBufferFrames));
                saveUiPreferences(*shellState);
                shellState->statusMessage =
                    "audio buffer updated; playback output will restart";
            }
            ImGui::SeparatorText("Project Safety");
            int autosaveInterval = shellState->autosaveIntervalMinutes;
            if (ImGui::InputInt(
                    "Autosave Interval (min)", &autosaveInterval)) {
                shellState->autosaveIntervalMinutes =
                    std::clamp(autosaveInterval, 1, 120);
                setLegacyStateOverride(
                    shellState,
                    "autosaveIntervalMinutes",
                    shellState->autosaveIntervalMinutes);
                shellState->nextAutosaveAt =
                    std::chrono::steady_clock::now() +
                    std::chrono::minutes(
                        shellState->autosaveIntervalMinutes);
                shellState->statusMessage =
                    "autosave interval changed; save the project to keep it";
            }
            int autosaveBackups = shellState->autosaveMaxBackups;
            if (ImGui::InputInt(
                    "Autosave Backups", &autosaveBackups)) {
                shellState->autosaveMaxBackups =
                    std::clamp(autosaveBackups, 1, 200);
                setLegacyStateOverride(
                    shellState,
                    "autosaveMaxBackups",
                    shellState->autosaveMaxBackups);
                shellState->statusMessage =
                    "autosave retention changed; save the project to keep it";
            }
            int historyEntries = shellState->historyMaxEntries;
            if (ImGui::InputInt("History Entries", &historyEntries)) {
                shellState->historyMaxEntries =
                    std::clamp(historyEntries, 10, 500);
                setLegacyStateOverride(
                    shellState,
                    "historyMaxEntries",
                    shellState->historyMaxEntries);
                shellState->statusMessage =
                    "history retention changed; save the project to keep it";
            }
            int historyMegabytes = shellState->historyMaxMegabytes;
            if (ImGui::InputInt(
                    "History Size (MB)", &historyMegabytes)) {
                shellState->historyMaxMegabytes =
                    std::clamp(historyMegabytes, 1, 256);
                setLegacyStateOverride(
                    shellState,
                    "historyMaxMegabytes",
                    shellState->historyMaxMegabytes);
                shellState->statusMessage =
                    "history size changed; save the project to keep it";
            }
            ImGui::TextDisabled(
                "Autosave backups use Qt-compatible state_backup_*.json files. "
                "History limits apply on the next project save.");
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void drawStatusBar(const ShellState& shellState, const jcut::EditorDocumentCore& snapshot)
{
    const jcut::ImGuiAudioStatus audioStatus = shellState.audioRuntime.status();
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
    if (!audioStatus.message.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::TextUnformatted(audioStatus.message.c_str());
    }
    ImGui::End();
}

void drawCloseConfirmation(ShellState* shellState, bool* exitRequested)
{
    if (shellState->closeConfirmationRequested) {
        ImGui::OpenPopup("Unsaved Changes");
        shellState->closeConfirmationRequested = false;
    }
    if (!ImGui::BeginPopupModal(
            "Unsaved Changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    ImGui::TextUnformatted("Save changes before closing JCut?");
    if (ImGui::Button("Save and Close")) {
        if (saveCurrentDocument(shellState)) {
            *exitRequested = true;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard")) {
        *exitRequested = true;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
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
        shellState.documentPath = argv[1];
        shellState.projectRootPath = pathString(fs::path(shellState.documentPath).parent_path());
        shellState.mediaRootDirectory = shellState.projectRootPath;
        jcut::EditorDocumentCore document = *loadedDocument;
        jcut::standalone_render::probeUnknownAudioPresence(
            &document, shellState.mediaRootDirectory);
        {
            std::lock_guard<std::mutex> lock(shellState.runtimeMutex);
            shellState.runtime = jcut::EditorRuntime::fromDocument(document);
        }
        std::snprintf(shellState.mediaRootPath.data(),
                      shellState.mediaRootPath.size(),
                      "%s",
                      shellState.mediaRootDirectory.c_str());
        shellState.preferencesPath = pathString(fs::path(shellState.documentPath).parent_path() /
                                                (fs::path(shellState.documentPath).filename().string() +
                                                 ".imgui_prefs.json"));
        shellState.lastSavedSnapshotJson = snapshotJson(document);
        shellState.lastSavedLegacyExtensionSignature =
            legacyExtensionSignature(shellState);
        shellState.statusMessage = "document loaded";
        std::snprintf(shellState.exportOutputPath.data(),
                      shellState.exportOutputPath.size(),
                      "%s",
                      document.exportRequest.outputPath.c_str());
    } else {
        std::string error;
        const std::optional<jcut::ImGuiProjectSession> session =
            jcut::loadActiveImGuiProjectSession(&error);
        if (!session.has_value()) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        shellState.projectRootPath = session->rootDirPath;
        shellState.mediaRootDirectory = session->mediaRootPath.empty()
            ? session->rootDirPath
            : session->mediaRootPath;
        jcut::EditorDocumentCore document = session->document;
        jcut::standalone_render::probeUnknownAudioPresence(
            &document, shellState.mediaRootDirectory);
        {
            std::lock_guard<std::mutex> lock(shellState.runtimeMutex);
            shellState.runtime = jcut::EditorRuntime::fromDocument(document);
        }
        shellState.projectId = session->projectId;
        shellState.statePath = session->statePath;
        shellState.historyPath = session->historyPath;
        std::snprintf(shellState.mediaRootPath.data(),
                      shellState.mediaRootPath.size(),
                      "%s",
                      shellState.mediaRootDirectory.c_str());
        shellState.preferencesPath = pathString(fs::path(shellState.statePath).parent_path() /
                                                "imgui_prefs.json");
        shellState.legacyStateRoot = session->legacyStateRoot;
        shellState.legacyStateOverrides = nlohmann::json::object();
        shellState.usesQtProjectStorage = true;
        shellState.lastSavedSnapshotJson = snapshotJson(document);
        shellState.lastSavedLegacyExtensionSignature =
            legacyExtensionSignature(shellState);
        shellState.statusMessage = "active Qt project loaded";
        std::snprintf(shellState.exportOutputPath.data(),
                      shellState.exportOutputPath.size(),
                      "%s",
                      document.exportRequest.outputPath.c_str());
    }
    reloadProjectPreferenceState(&shellState);
    shellState.lastSavedLegacyExtensionSignature =
        legacyExtensionSignature(shellState);
    loadUiPreferences(&shellState);
    shellState.audioRuntime.setBufferFrames(
        static_cast<unsigned int>(shellState.audioBufferFrames));
    shellState.layoutIniPath = pathString(
        fs::path(shellState.preferencesPath).parent_path() / "imgui_layout.ini");

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
    ImGui::GetIO().IniFilename = shellState.layoutIniPath.c_str();
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

    bool exitRequested = false;
    while (!exitRequested) {
        platform.pollEvents();
        if (platform.shouldClose()) {
            platform.closeRequested = false;
            jcut::EditorDocumentCore closeSnapshot;
            {
                std::lock_guard<std::mutex> lock(shellState.runtimeMutex);
                closeSnapshot = shellState.runtime.snapshot();
            }
            if (documentIsDirty(shellState, closeSnapshot)) {
                shellState.closeConfirmationRequested = true;
            } else {
                exitRequested = true;
                continue;
            }
        }
        const auto now = std::chrono::steady_clock::now();
        const std::chrono::duration<double> delta = now - previousTick;
        previousTick = now;
        const bool controlPreviewRefresh =
            shellState.uiPreviewRefreshRequested.exchange(false, std::memory_order_acq_rel);
        int previousFrame = 0;
        int currentFrame = 0;
        const jcut::ImGuiAudioStatus preTickAudioStatus =
            shellState.audioRuntime.status();
        {
            std::lock_guard<std::mutex> lock(shellState.runtimeMutex);
            const jcut::EditorDocumentCore beforeTick = shellState.runtime.snapshot();
            previousFrame = beforeTick.transport.currentFrame;
            const bool holdForAudioWarmup =
                beforeTick.transport.playbackActive &&
                preTickAudioStatus.hasPlayableAudio &&
                !preTickAudioStatus.outputUnavailable &&
                (preTickAudioStatus.buffering ||
                 !preTickAudioStatus.playbackStarted);
            if (!holdForAudioWarmup) {
                shellState.runtime.tick({delta.count()});
            }
            currentFrame = shellState.runtime.snapshot().transport.currentFrame;
        }
        if (currentFrame != previousFrame || controlPreviewRefresh) {
            requestPreviewRender(&shellState);
        }
        uploadPreviewTexture(&shellState, &vulkanShell);

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
        runAutosaveIfDue(&shellState, snapshot);
        handleKeyboardShortcuts(&shellState, snapshot);
        {
            std::lock_guard<std::mutex> lock(shellState.runtimeMutex);
            snapshot = shellState.runtime.snapshot();
        }
        shellState.audioRuntime.synchronize(
            snapshot,
            shellState.mediaRootDirectory.empty()
                ? shellState.projectRootPath
                : shellState.mediaRootDirectory);
        drawMenuBar(&shellState, snapshot);
        drawMediaPanel(&shellState, snapshot);
        drawPreviewPanel(&shellState, snapshot);
        drawTimelinePanel(&shellState, snapshot);
        drawInspectorPanel(&shellState, snapshot);
        drawProjectLifecyclePopup(&shellState);
        finishRuntimeHistoryTransactionIfIdle(&shellState);
        shellState.resetLayoutRequested = false;
        drawStatusBar(shellState, snapshot);
        drawCloseConfirmation(&shellState, &exitRequested);

        ImGui::Render();
        vulkanShell.renderDrawData(ImGui::GetDrawData());
        vulkanShell.present();
    }

    vulkanShell.shutdown();
    shellState.audioRuntime.shutdown();
    ImGui::SaveIniSettingsToDisk(shellState.layoutIniPath.c_str());
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
