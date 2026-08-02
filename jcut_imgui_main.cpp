#include "editor_runtime.h"
#include "jcut_imgui_application.h"
#include "ai_credential_store_core.h"
#include "ai_gateway_core.h"
#include "birefnet_job_core.h"
#include "editor_auto_oppose_core.h"
#include "editor_document_core_json.h"
#include "editor_grading_core.h"
#include "editor_media_presence_core.h"
#include "editor_scale_to_fill.h"
#include "editor_timeline_mapping_core.h"
#include "face_artifact_core.h"
#include "face_avatar_crop_core.h"
#include "face_processing_job_core.h"
#include "imgui_audio_runtime.h"
#include "imgui_gpu_renderer_bridge.h"
#include "image_sequence_directory.h"
#include "mask_sidecar_core.h"
#include "prompt_mask_job_core.h"
#include "imgui_project_io.h"
#include "imgui_vulkan_frame_importer.h"
#include "vulkan_hardware_frame_import_core.h"
#include "preview_resize_core.h"
#include "timeline_snap_core.h"
#include "proxy_path_core.h"
#include "proxy_generation_job_core.h"
#include "render_contract_json.h"
#include "runtime_control_server.h"
#include "speaker_section_core.h"
#include "speaker_section_export_core.h"
#include "speaker_title_core.h"
#include "standalone_export_renderer.h"
#include "standalone_preview_renderer.h"
#include "standalone_timeline_renderer.h"
#include "timeline_viewport_core.h"
#include "transcript_cut_session_core.h"
#include "transcript_document_mutation_core.h"
#include "transcript_mining_core.h"
#include "transcription_job_core.h"

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
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <optional>
#include <set>
#include <spawn.h>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <mutex>
#include <limits>
#include <system_error>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

extern char** environ;

namespace {

namespace fs = std::filesystem;

bool openDesktopPath(const fs::path& path, std::string* errorOut = nullptr)
{
    if (errorOut) errorOut->clear();
    if (path.empty()) {
        if (errorOut) *errorOut = "No path was provided.";
        return false;
    }
    const std::string pathText = path.lexically_normal().string();
    std::array<char*, 3> arguments{
        const_cast<char*>("xdg-open"),
        const_cast<char*>(pathText.c_str()),
        nullptr,
    };
    pid_t child = -1;
    const int spawnResult = posix_spawnp(
        &child, "xdg-open", nullptr, nullptr, arguments.data(), environ);
    if (spawnResult != 0) {
        if (errorOut) {
            *errorOut =
                "Could not launch the desktop file browser (error " +
                std::to_string(spawnResult) + ").";
        }
        return false;
    }
    std::thread([child]() {
        int status = 0;
        while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
    }).detach();
    return true;
}

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
    Rotate,
};

enum class InspectorDeleteTargetKind {
    None,
    ClipKeyframe,
    TitleKeyframe,
    SyncMarker,
    TranscriptWord,
};

struct InspectorDeleteTarget {
    InspectorDeleteTargetKind kind = InspectorDeleteTargetKind::None;
    int clipId = -1;
    jcut::EditorKeyframeChannel channel =
        jcut::EditorKeyframeChannel::Grading;
    std::int64_t frame = -1;
    std::string markerClipId;
    bool markerSkipFrame = false;
    std::string transcriptPath;
    int transcriptSegmentIndex = -1;
    int transcriptWordIndex = -1;
    std::uint64_t documentGeneration = 0;
    std::uint64_t focusedUiFrame = 0;
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

constexpr float kTimelineRowHeight = 34.0f;
constexpr float kTimelineLabelWidth = 180.0f;
constexpr float kTimelineClipHeight = 24.0f;
constexpr float kTimelineTrackPadding = 12.0f;
constexpr float kTimelineTopPadding = 10.0f;
constexpr float kTimelineRulerHeight = 28.0f;
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
    bool speakerSectionsExpanded = false;
    bool speakerSectionOptionsPopupRequested = false;
    std::string speakerSectionOptionsSpeakerId;
    std::int64_t speakerSectionOptionsStartFrame = -1;
    std::int64_t speakerSectionOptionsEndFrame = -1;
    std::size_t speakerSectionOptionsWordCount = 0;
    jcut::SpeakerSectionOptionsCore speakerSectionOptionsDraft;
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
    bool faceJobControlWindow = false;
    bool faceJobLivePreview = false;
    bool faceJobRestartFromScratch = false;
    bool faceJobUseProxySource = false;
    bool faceJobBenchmarkTopology = false;
    bool faceJobApplyClipGrading = false;
    int speakerTitleStyle = 0;
    double speakerTitleDurationSeconds = 3.0;
    double speakerTitleDelaySeconds = 0.35;
    bool speakerTitleShowAtSectionEnd = false;
    bool speakerTitleRespectSpeechFilterTiming = true;
    double speakerTitleCadenceSeconds = 0.0;
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

struct AutoOpposeJobResult {
    int clipId = 0;
    std::uint64_t documentGeneration = 0;
    int decodedSamples = 0;
    std::vector<jcut::EditorOpposeGradeEventCore> events;
    std::string message;
};

struct AiChatMessage {
    std::string role;
    std::string content;
};

struct AiActivityEntry {
    std::string time;
    std::string phase;
    std::string summary;
};

enum class AiTaskPurpose {
    Chat,
    CloudSpeakerMining,
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
    std::uint64_t dirtyGeneration = 1;
    std::uint64_t legacyStateGeneration = 1;
    mutable std::uint64_t dirtyCacheGeneration = 0;
    mutable std::uint64_t dirtyCacheLegacyGeneration = 0;
    mutable bool dirtyCacheValue = false;
    std::string lastSavedSnapshotJson;
    std::string statusMessage;
    jcut::ImGuiAudioRuntime audioRuntime;
    jcut::imgui_gpu::RendererBridge gpuRenderer;
    jcut::FaceProcessingJobController faceProcessingJob;
    jcut::masks::PromptMaskJobController promptMaskJob;
    jcut::ProxyGenerationJobController proxyGenerationJob;
    jcut::jobs::TranscriptionJobControllerCore transcriptionJob;
    jcut::jobs::BiRefNetJobControllerCore birefnetJob;
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
    std::string jobsTextPreviewLabel;
    std::string jobsTextPreviewPath;
    std::string jobsTextPreview;
    std::string jobsTextPreviewError;
    bool featureAiPanel = true;
    bool featureAiSpeakerCleanup = true;
    std::string aiGatewayBaseUrl =
        "https://ivwutugdrpugjqglxabw.supabase.co";
    std::string aiSessionToken;
    std::string aiRefreshToken;
    std::string aiUserId;
    std::string aiCredentialStatus;
    bool aiCredentialLoadAttempted = false;
    std::string aiSelectedModel = "deepseek-chat";
    int aiUsageBudgetCap = 200;
    int aiUsageRequests = 0;
    int aiUsageFailures = 0;
    int aiRequestTimeoutMs = 15000;
    int aiRequestRetries = 1;
    bool aiAccountRefreshRunning = false;
    std::future<jcut::ai::AccountSnapshotCore> aiAccountFuture;
    jcut::ai::AccountSnapshotCore aiAccount;
    bool aiTokenRefreshRunning = false;
    std::future<jcut::ai::RefreshedSessionCore> aiTokenRefreshFuture;
    bool aiBrowserLoginRunning = false;
    std::atomic_bool aiBrowserLoginCancelRequested{false};
    std::future<jcut::ai::BrowserLoginCore> aiBrowserLoginFuture;
    bool aiCheckoutRunning = false;
    std::future<jcut::ai::CheckoutLaunchCore> aiCheckoutFuture;
    bool aiTaskRunning = false;
    AiTaskPurpose aiTaskPurpose = AiTaskPurpose::Chat;
    std::string aiTaskTranscriptSourceKey;
    std::future<jcut::ai::TaskResponseCore> aiTaskFuture;
    std::vector<std::chrono::steady_clock::time_point>
        aiRecentRequestTimes;
    std::string aiChatPrompt;
    std::vector<AiChatMessage> aiChatMessages;
    std::vector<AiActivityEntry> aiActivityEntries;
    bool aiAvatarRunning = false;
    std::future<jcut::ai::RemoteImageCore> aiAvatarFuture;
    std::string aiAvatarRequestedUrl;
    std::string aiAvatarLoadedUrl;
    std::string aiAvatarError;
    std::string aiAvatarCachePath;
    ImTextureID aiAvatarTextureId = 0;
    jcut::core::SizeI aiAvatarSize{};
    std::string faceReferenceDesiredKey;
    std::string faceReferenceSourcePath;
    std::vector<jcut::FaceContinuityTrackCore>
        faceReferenceTracks;
    std::string faceReferencePendingKey;
    std::string faceReferenceLoadedKey;
    std::string faceReferenceError;
    bool faceReferenceRunning = false;
    std::future<
        jcut::standalone_render::StandaloneDecodedFrameResult>
        faceReferenceFuture;
    ImTextureID faceReferenceTextureId = 0;
    jcut::core::SizeI faceReferenceSize{};
    std::string sectionAvatarDesiredKey;
    std::string sectionAvatarSourcePath;
    std::vector<jcut::FaceContinuityTrackCore>
        sectionAvatarTracks;
    std::string sectionAvatarPendingKey;
    std::string sectionAvatarLoadedKey;
    std::string sectionAvatarError;
    bool sectionAvatarRunning = false;
    std::future<
        jcut::standalone_render::StandaloneDecodedFrameResult>
        sectionAvatarFuture;
    ImTextureID sectionAvatarTextureId = 0;
    jcut::core::SizeI sectionAvatarSize{};
    int selectedPipelineStage = 0;
    std::string requestedInspectorTab;
    jcut::EditorAutoOpposeSettingsCore autoOpposeSettings;
    std::future<AutoOpposeJobResult> autoOpposeFuture;
    bool autoOpposeRunning = false;
    int autoOpposeClipId = 0;
    std::string mediaGalleryPath;
    std::string mediaHoveredPath;
    std::string mediaSelectedPath;
    ImTextureID mediaThumbnailTextureId = 0;
    jcut::core::SizeI mediaThumbnailSize{};
    std::string mediaThumbnailLoadedPath;
    std::string mediaThumbnailPendingPath;
    std::string mediaThumbnailError;
    bool mediaThumbnailRunning = false;
    std::future<
        jcut::standalone_render::StandaloneDecodedFrameResult>
        mediaThumbnailFuture;
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
    std::string audioOutputDeviceName;
    jcut::DecoderPolicySettingsCore decoderPolicy;
    jcut::DecoderPolicySettingsCore previewDecoderPolicy;
    jcut::DecoderPolicySettingsCore exportDecoderPolicy;
    std::future<jcut::standalone_render::StandaloneDecodeBenchmarkResult>
        decodeBenchmarkFuture;
    bool decodeBenchmarkRunning = false;
    jcut::standalone_render::StandaloneDecodeBenchmarkResult
        decodeBenchmarkResult;
    std::chrono::steady_clock::time_point nextAutosaveAt =
        std::chrono::steady_clock::now() + std::chrono::minutes(5);
    int titleDraftClipId = -1;
    jcut::EditorTitleKeyframe titleDraft;
    InspectorKeyframeDraft keyframeDraft;
    InspectorDeleteTarget inspectorDeleteTarget;
    std::uint64_t uiFrameCounter = 0;
    TranscriptInspectorCache transcriptCache;
    bool transcriptDeletePopupRequested = false;
    std::unordered_map<std::string, jcut::TranscriptFileStamp>
        transcriptHistoryExpectedStamps;
    RenderSyncMarkerDraft renderSyncMarkerDraft;
    TimelineDragMode timelineDragMode = TimelineDragMode::None;
    TimelineToolMode timelineToolMode = TimelineToolMode::Select;
    bool timelineSnappingEnabled = true;
    float timelinePixelsPerFrame =
        jcut::timeline_viewport::kDefaultPixelsPerFrame;
    std::int64_t timelineFrameOffset = 0;
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
    int timelineContextClickFrame = 0;
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
    int promptMaskSourceClipId = -1;
    int promptMaskLastState = -1;
    int transcriptionLastState = -1;
    std::string transcriptionStdinDraft;
    int birefnetLastState = -1;
    int birefnetSourceClipId = -1;
    std::string birefnetModel = "ZhengPeng7/BiRefNet-matting";
    std::string birefnetRevision =
        "57f9f68b43ba337c75762b14cf3075d659007268";
    std::string birefnetModelCachePath;
    std::string birefnetRuntimeCachePath;
    int birefnetDevice = 0;
    bool birefnetFp16 = true;
    bool birefnetDockerRoot = false;
    bool birefnetRestart = false;
    float birefnetAlphaTolerancePercent = 0.0f;
    ImTextureID birefnetLivePreviewTextureId = 0;
    jcut::core::SizeI birefnetLivePreviewSize{};
    std::string birefnetLivePreviewLoadedPath;
    std::uintmax_t birefnetLivePreviewLoadedSize = 0;
    fs::file_time_type birefnetLivePreviewLoadedTime{};
    bool birefnetLivePreviewHasStamp = false;
    std::string birefnetLivePreviewError;
    std::chrono::steady_clock::time_point
        nextBiRefNetLivePreviewRefresh{};
    std::string promptMaskPrompt =
        "a microphone mounted on a microphone stand";
    std::string promptMaskModelCachePath;
    std::string promptMaskRuntimeCachePath;
    int promptMaskScaleWidth = 0;
    int promptMaskPrescaleWidth = 0;
    float promptMaskExtractFps = 0.0f;
    int promptMaskFrameFormat = 0;
    bool promptMaskCompileModel = false;
    bool promptMaskVideoMode = false;
    bool promptMaskWriteBinaryMasks = true;
    bool promptMaskUnionCurrent = false;
    bool promptMaskWritePreviewFrames = false;
    bool promptMaskExportCenters = false;
    bool promptMaskDockerRoot = false;
    bool promptMaskRestart = false;
    jcut::EditorDocumentCore previewDocument;
    std::string previewRootDirectory;
    jcut::standalone_render::PreviewRenderResult previewResult;
    ImTextureID previewTextureId = 0;
    ImTextureID previewOverlayTextureId = 0;
    jcut::core::SizeI previewOverlaySize{};
    int previewOverlayX = 0;
    int previewOverlayY = 0;
    bool previewHardwarePresentationTransformValid = false;
    jcut::EditorTransformKeyframe
        previewHardwarePresentationTransform;
    double previewHardwarePresentationOpacity = 1.0;
    jcut::core::SizeI previewHardwareSourceSize{};
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
    struct QueuedExport {
        jcut::EditorDocumentCore document;
        std::string label;
    };
    std::vector<QueuedExport> exportQueue;
    std::size_t exportQueueTotal = 0;
    std::size_t exportQueueCurrent = 0;
    std::size_t exportQueueCompleted = 0;
    std::size_t exportQueueFailed = 0;
    std::string exportQueueLabel;
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

std::string readTextFileTail(const fs::path& path,
                             std::size_t maximumBytes,
                             std::string* errorOut)
{
    if (errorOut) errorOut->clear();
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        if (errorOut) *errorOut = "file could not be opened";
        return {};
    }
    stream.seekg(0, std::ios::end);
    const std::streamoff byteCount = stream.tellg();
    if (byteCount < 0) {
        if (errorOut) *errorOut = "file size could not be read";
        return {};
    }
    const std::streamoff readOffset =
        byteCount > static_cast<std::streamoff>(maximumBytes)
        ? byteCount - static_cast<std::streamoff>(maximumBytes)
        : 0;
    stream.seekg(readOffset, std::ios::beg);
    std::string content(
        (std::istreambuf_iterator<char>(stream)),
        std::istreambuf_iterator<char>());
    if (!stream.eof() && stream.fail()) {
        if (errorOut) *errorOut = "file could not be read";
        return {};
    }
    if (readOffset > 0) {
        content.insert(
            0,
            "[showing the final " + std::to_string(maximumBytes) +
                " bytes]\n");
    }
    return content;
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

fs::path defaultApplicationCachePath()
{
    if (const char* value = std::getenv("XDG_CACHE_HOME");
        value && *value) {
        return fs::path(value) / "PanelTalkEditor" / "JCut";
    }
    if (const char* value = std::getenv("HOME"); value && *value) {
        return fs::path(value) / ".cache" / "PanelTalkEditor" / "JCut";
    }
    return fs::current_path() / ".cache";
}

std::optional<fs::path> sam3ScriptPath()
{
    const fs::path executable = executableDirPath();
    return firstExistingPath({
        fs::current_path() / "sam3.sh",
        executable / "sam3.sh",
        executable.parent_path() / "sam3.sh",
    });
}

void loadUiPreferences(ShellState* shellState)
{
    if (shellState->promptMaskModelCachePath.empty()) {
        shellState->promptMaskModelCachePath =
            (defaultApplicationCachePath() / "sam3" / "hf").string();
    }
    if (shellState->promptMaskRuntimeCachePath.empty()) {
        shellState->promptMaskRuntimeCachePath =
            (defaultApplicationCachePath() / "sam3" / "runtime").string();
    }
    if (shellState->birefnetModelCachePath.empty()) {
        shellState->birefnetModelCachePath =
            (defaultApplicationCachePath() / "birefnet" / "hf").string();
    }
    if (shellState->birefnetRuntimeCachePath.empty()) {
        shellState->birefnetRuntimeCachePath =
            (defaultApplicationCachePath() / "birefnet" / "runtime").string();
    }
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
        shellState->timelinePixelsPerFrame = std::clamp(
            root.value(
                "timelinePixelsPerFrame",
                jcut::timeline_viewport::kDefaultPixelsPerFrame),
            0.0001f,
            jcut::timeline_viewport::kMaximumPixelsPerFrame);
        shellState->audioBufferFrames = std::clamp(
            root.value("audioBufferFrames", 1024), 64, 8192);
        shellState->audioOutputDeviceName =
            root.value("audioOutputDeviceName", std::string{});
        jcut::DecodePreferenceCore decodePreference =
            jcut::DecodePreferenceCore::Auto;
        (void)jcut::parseDecodePreferenceCore(
            root.value("standaloneDecodePreference", std::string("auto")),
            &decodePreference);
        shellState->decoderPolicy.decodePreference = decodePreference;
        jcut::DecodeHardwareDeviceCore hardwareDevice =
            jcut::DecodeHardwareDeviceCore::Auto;
        (void)jcut::parseDecodeHardwareDeviceCore(
            root.value(
                "standaloneDecodeHardwareDevice",
                std::string("auto")),
            &hardwareDevice);
        shellState->decoderPolicy.hardwareDevice = hardwareDevice;
        shellState->promptMaskModelCachePath =
            root.value("sam3ModelCachePath", std::string{});
        shellState->promptMaskRuntimeCachePath =
            root.value("sam3RuntimeCachePath", std::string{});
        shellState->promptMaskScaleWidth =
            std::clamp(root.value("sam3ScaleWidth", 0), 0, 8192);
        shellState->promptMaskPrescaleWidth =
            std::clamp(root.value("sam3PrescaleWidth", 0), 0, 8192);
        shellState->promptMaskExtractFps =
            std::clamp(root.value("sam3ExtractFps", 0.0f), 0.0f, 240.0f);
        shellState->promptMaskFrameFormat =
            root.value("sam3IntermediateFramesFormat", std::string("jpg")) ==
                    "png"
                ? 1
                : 0;
        shellState->promptMaskCompileModel =
            root.value("sam3CompileModel", false);
        shellState->birefnetModel =
            root.value("birefnetModel", shellState->birefnetModel);
        shellState->birefnetRevision =
            root.value("birefnetRevision", shellState->birefnetRevision);
        shellState->birefnetModelCachePath =
            root.value("birefnetModelCachePath", std::string{});
        shellState->birefnetRuntimeCachePath =
            root.value("birefnetRuntimeCachePath", std::string{});
        shellState->birefnetDevice =
            std::clamp(root.value("birefnetDevice", 0), 0, 1);
        shellState->birefnetFp16 =
            root.value("birefnetFp16", true);
        shellState->birefnetDockerRoot =
            root.value("birefnetDockerRoot", false);
        shellState->birefnetAlphaTolerancePercent = std::clamp(
            root.value("birefnetAlphaTolerancePercent", 0.0f),
            0.0f,
            99.0f);
    } catch (...) {
        shellState->uiFontSize = kDefaultUiFontSize;
        shellState->timelinePixelsPerFrame =
            jcut::timeline_viewport::kDefaultPixelsPerFrame;
        shellState->audioBufferFrames = 1024;
        shellState->audioOutputDeviceName.clear();
        shellState->decoderPolicy.decodePreference =
            jcut::DecodePreferenceCore::Auto;
        shellState->decoderPolicy.hardwareDevice =
            jcut::DecodeHardwareDeviceCore::Auto;
        shellState->promptMaskModelCachePath.clear();
        shellState->promptMaskRuntimeCachePath.clear();
        shellState->promptMaskScaleWidth = 0;
        shellState->promptMaskPrescaleWidth = 0;
        shellState->promptMaskExtractFps = 0.0f;
        shellState->promptMaskFrameFormat = 0;
        shellState->promptMaskCompileModel = false;
    }
    if (shellState->promptMaskModelCachePath.empty()) {
        shellState->promptMaskModelCachePath =
            (defaultApplicationCachePath() / "sam3" / "hf").string();
    }
    if (shellState->promptMaskRuntimeCachePath.empty()) {
        shellState->promptMaskRuntimeCachePath =
            (defaultApplicationCachePath() / "sam3" / "runtime").string();
    }
    if (shellState->birefnetModelCachePath.empty()) {
        shellState->birefnetModelCachePath =
            (defaultApplicationCachePath() / "birefnet" / "hf").string();
    }
    if (shellState->birefnetRuntimeCachePath.empty()) {
        shellState->birefnetRuntimeCachePath =
            (defaultApplicationCachePath() / "birefnet" / "runtime").string();
    }
}

void saveUiPreferences(const ShellState& shellState)
{
    if (shellState.preferencesPath.empty()) {
        return;
    }
    const nlohmann::json root{
        {"uiFontSize", shellState.uiFontSize},
        {"timelinePixelsPerFrame", shellState.timelinePixelsPerFrame},
        {"audioBufferFrames", shellState.audioBufferFrames},
        {"audioOutputDeviceName", shellState.audioOutputDeviceName},
        {"standaloneDecodePreference",
         jcut::decodePreferenceCoreName(
             shellState.decoderPolicy.decodePreference)},
        {"standaloneDecodeHardwareDevice",
         jcut::decodeHardwareDeviceCoreName(
             shellState.decoderPolicy.hardwareDevice)},
        {"sam3ModelCachePath", shellState.promptMaskModelCachePath},
        {"sam3RuntimeCachePath", shellState.promptMaskRuntimeCachePath},
        {"sam3ScaleWidth", shellState.promptMaskScaleWidth},
        {"sam3PrescaleWidth", shellState.promptMaskPrescaleWidth},
        {"sam3ExtractFps", shellState.promptMaskExtractFps},
        {"sam3IntermediateFramesFormat",
         shellState.promptMaskFrameFormat == 1 ? "png" : "jpg"},
        {"sam3CompileModel", shellState.promptMaskCompileModel},
        {"birefnetModel", shellState.birefnetModel},
        {"birefnetRevision", shellState.birefnetRevision},
        {"birefnetModelCachePath", shellState.birefnetModelCachePath},
        {"birefnetRuntimeCachePath", shellState.birefnetRuntimeCachePath},
        {"birefnetDevice", shellState.birefnetDevice},
        {"birefnetFp16", shellState.birefnetFp16},
        {"birefnetDockerRoot", shellState.birefnetDockerRoot},
        {"birefnetAlphaTolerancePercent",
         shellState.birefnetAlphaTolerancePercent},
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

#include "jcut_imgui_project_workflows.h"
#include "jcut_imgui_timeline_workflows.h"
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

#include "jcut_imgui_x11_platform.h"

#include "jcut_imgui_vulkan_host.h"

#include "jcut_imgui_texture_workflows.h"
#include "jcut_imgui_keyboard_workflow.h"
#include "jcut_imgui_menu_panel.h"

#include "jcut_imgui_media_panel.h"

#include "jcut_imgui_preview_panel.h"

#include "jcut_imgui_timeline_panel.h"

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

bool effectPresetIsMirrorGeometry(std::string_view presetId)
{
    return presetId == "mirror_ring" ||
        presetId == "kaleidoscope" ||
        presetId == "quad_mirror" ||
        presetId == "infinite_mirror" ||
        presetId == "tessellation" ||
        presetId == "hexagonal_prism" ||
        presetId == "droste" ||
        presetId == "recursive_zoom_tile";
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
        presetId == "sobel_edges" ||
        presetId == "neon_glow" ||
        effectPresetIsMirrorGeometry(presetId) ||
        effectPresetIsSpeakerMask(presetId);
}

bool effectPresetUsesTilingParameters(std::string_view presetId)
{
    return presetId == "source_tile";
}

bool effectPresetUsesSpacingParameter(std::string_view presetId)
{
    return effectPresetUsesTilingParameters(presetId) ||
        effectPresetIsMirrorGeometry(presetId) ||
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

void markInspectorDeleteTargetForLastItem(
    ShellState* shellState,
    InspectorDeleteTargetKind kind,
    int clipId,
    jcut::EditorKeyframeChannel channel,
    std::int64_t frame)
{
    if (!ImGui::IsItemFocused()) {
        return;
    }
    shellState->inspectorDeleteTarget = {
        kind,
        clipId,
        channel,
        frame,
        {},
        false,
        {},
        -1,
        -1,
        shellState->documentGeneration,
        shellState->uiFrameCounter};
}

void markSyncDeleteTargetForLastItem(
    ShellState* shellState,
    const jcut::EditorRenderSyncMarker& marker)
{
    if (!ImGui::IsItemFocused()) {
        return;
    }
    shellState->inspectorDeleteTarget = {
        InspectorDeleteTargetKind::SyncMarker,
        -1,
        jcut::EditorKeyframeChannel::Transform,
        marker.frame,
        marker.clipId,
        marker.skipFrame,
        {},
        -1,
        -1,
        shellState->documentGeneration,
        shellState->uiFrameCounter};
}

void markTranscriptDeleteTargetForLastItem(
    ShellState* shellState,
    int clipId,
    const jcut::TranscriptCutSession& transcript,
    const jcut::TranscriptRow& row)
{
    if (!transcript.activeCutMutable ||
        !ImGui::IsItemFocused()) {
        return;
    }
    shellState->inspectorDeleteTarget = {
        InspectorDeleteTargetKind::TranscriptWord,
        clipId,
        jcut::EditorKeyframeChannel::Transform,
        -1,
        {},
        false,
        transcript.activePath,
        row.word.segmentIndex,
        row.word.wordIndex,
        shellState->documentGeneration,
        shellState->uiFrameCounter};
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
                                 bool threePointLock,
                                 bool smoothingEnabled = true)
{
    if (!points) {
        return false;
    }
    *points = jcut::sanitizeEditorGradingCurve(*points);

    bool curveChanged = false;
    ImGui::PushID(channelLabel);
    if (ImGui::TreeNode(channelLabel)) {
        if (threePointLock) {
            ImGui::TextDisabled(
                "Point X positions follow Lift / Gamma / Gain while three-point lock is enabled");
        }

        constexpr double kCurveXMinimum = 0.0;
        constexpr double kCurveXMaximum = 1.0;
        constexpr double kCurveYMinimum = -1.0;
        constexpr double kCurveYMaximum = 2.0;

        // Match the Qt curve widget's interaction model: click the plot to add
        // a point, drag handles with fixed endpoint X positions, and
        // right-click an interior point to remove it.
        const ImVec2 canvasSize(
            std::max(180.0f, ImGui::GetContentRegionAvail().x),
            180.0f);
        const ImVec2 canvasOrigin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton(
            "CurveCanvas",
            canvasSize,
            ImGuiButtonFlags_MouseButtonLeft |
                ImGuiButtonFlags_MouseButtonRight);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 canvasEnd(
            canvasOrigin.x + canvasSize.x,
            canvasOrigin.y + canvasSize.y);
        drawList->AddRectFilled(
            canvasOrigin, canvasEnd, IM_COL32(16, 22, 30, 255));
        drawList->AddRect(
            canvasOrigin, canvasEnd, IM_COL32(70, 84, 102, 255));
        for (int gridIndex = 1; gridIndex < 4; ++gridIndex) {
            const float fraction = static_cast<float>(gridIndex) / 4.0f;
            drawList->AddLine(
                ImVec2(canvasOrigin.x + canvasSize.x * fraction, canvasOrigin.y),
                ImVec2(canvasOrigin.x + canvasSize.x * fraction, canvasEnd.y),
                IM_COL32(55, 66, 80, 150));
            drawList->AddLine(
                ImVec2(canvasOrigin.x, canvasOrigin.y + canvasSize.y * fraction),
                ImVec2(canvasEnd.x, canvasOrigin.y + canvasSize.y * fraction),
                IM_COL32(55, 66, 80, 150));
        }
        const auto pointToCanvas = [&](const jcut::EditorPoint& point) {
            const double displayY =
                0.5 - ((point.y - point.x) * 0.5);
            return ImVec2(
                canvasOrigin.x +
                    static_cast<float>(point.x) * canvasSize.x,
                canvasOrigin.y +
                    static_cast<float>(displayY) * canvasSize.y);
        };
        const auto canvasToPoint = [&](const ImVec2& position) {
            const double x = std::clamp(
                static_cast<double>((position.x - canvasOrigin.x) /
                                    canvasSize.x),
                0.0,
                1.0);
            const double displayY = std::clamp(
                static_cast<double>((position.y - canvasOrigin.y) /
                                    canvasSize.y),
                0.0,
                1.0);
            return jcut::EditorPoint{
                x,
                std::clamp(
                    x + ((0.5 - displayY) * 2.0), -1.0, 2.0)};
        };
        drawList->PushClipRect(canvasOrigin, canvasEnd, true);
        const ImU32 curveColor =
            std::strcmp(channelLabel, "Red") == 0
            ? IM_COL32(244, 92, 92, 255)
            : (std::strcmp(channelLabel, "Green") == 0
                   ? IM_COL32(90, 220, 130, 255)
                   : (std::strcmp(channelLabel, "Blue") == 0
                          ? IM_COL32(90, 150, 245, 255)
                          : IM_COL32(232, 205, 90, 255)));
        ImVec2 previousCurvePoint = pointToCanvas(jcut::EditorPoint{
            0.0,
            jcut::sampleEditorGradingCurveAt(
                *points, 0.0, smoothingEnabled)});
        constexpr int kCurveSegments = 128;
        for (int segment = 1; segment <= kCurveSegments; ++segment) {
            const double x =
                static_cast<double>(segment) / kCurveSegments;
            const ImVec2 curvePoint = pointToCanvas(jcut::EditorPoint{
                x,
                jcut::sampleEditorGradingCurveAt(
                    *points, x, smoothingEnabled)});
            drawList->AddLine(
                previousCurvePoint, curvePoint, curveColor, 2.0f);
            previousCurvePoint = curvePoint;
        }

        ImGuiStorage* storage = ImGui::GetStateStorage();
        const ImGuiID activePointStorageId =
            ImGui::GetID("ActiveCurvePoint");
        int activePoint = storage->GetInt(activePointStorageId, -1);
        const ImVec2 mousePosition = ImGui::GetIO().MousePos;
        const auto nearestPoint = [&]() {
            int nearest = -1;
            float nearestDistanceSquared = 9.0f * 9.0f;
            for (std::size_t index = 0; index < points->size(); ++index) {
                const ImVec2 handle = pointToCanvas(points->at(index));
                const float dx = handle.x - mousePosition.x;
                const float dy = handle.y - mousePosition.y;
                const float distanceSquared = dx * dx + dy * dy;
                if (distanceSquared <= nearestDistanceSquared) {
                    nearestDistanceSquared = distanceSquared;
                    nearest = static_cast<int>(index);
                }
            }
            return nearest;
        };
        const bool canvasHovered = ImGui::IsItemHovered();
        if (canvasHovered &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Right) &&
            !threePointLock) {
            const int hit = nearestPoint();
            if (hit > 0 &&
                hit + 1 < static_cast<int>(points->size())) {
                points->erase(points->begin() + hit);
                curveChanged = true;
                activePoint = -1;
            }
        }
        if (canvasHovered &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            activePoint = nearestPoint();
            if (activePoint < 0 && !threePointLock) {
                const jcut::EditorPoint added =
                    canvasToPoint(mousePosition);
                auto insertion = std::upper_bound(
                    points->begin(),
                    points->end(),
                    added.x,
                    [](double x, const jcut::EditorPoint& point) {
                        return x < point.x;
                    });
                activePoint = static_cast<int>(
                    std::distance(points->begin(), insertion));
                points->insert(insertion, added);
                curveChanged = true;
            }
        }
        if (activePoint >= 0 &&
            activePoint < static_cast<int>(points->size()) &&
            ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            jcut::EditorPoint dragged = canvasToPoint(mousePosition);
            if (activePoint == 0) {
                dragged.x = 0.0;
            } else if (activePoint + 1 ==
                       static_cast<int>(points->size())) {
                dragged.x = 1.0;
            } else if (threePointLock) {
                dragged.x = 0.5;
            } else {
                dragged.x = std::clamp(
                    dragged.x,
                    points->at(static_cast<std::size_t>(activePoint - 1)).x +
                        0.001,
                    points->at(static_cast<std::size_t>(activePoint + 1)).x -
                        0.001);
            }
            points->at(static_cast<std::size_t>(activePoint)) = dragged;
            curveChanged = true;
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            activePoint = -1;
        }
        storage->SetInt(activePointStorageId, activePoint);
        for (std::size_t index = 0; index < points->size(); ++index) {
            const ImVec2 handle = pointToCanvas(points->at(index));
            drawList->AddCircleFilled(
                handle,
                static_cast<int>(index) == activePoint ? 6.0f : 4.5f,
                curveColor);
            drawList->AddCircle(
                handle, 6.0f, IM_COL32(245, 248, 252, 220));
        }
        drawList->PopClipRect();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                threePointLock
                    ? "Drag Lift, Gamma, or Gain points"
                    : "Click to add; drag to adjust; right-click an interior point to remove");
        }

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
                ImGui::BeginDisabled(endpoint || threePointLock);
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
                if (endpoint || threePointLock) {
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
        ImGui::BeginDisabled(threePointLock);
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
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset channel")) {
            *points = {{0.0, 0.0}, {1.0, 1.0}};
            curveChanged = true;
        }
        if (curveChanged) {
            *points = jcut::sanitizeEditorGradingCurve(*points);
        }

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
        const jcut::EditorTrackMediaPresenceCore trackPresence =
            jcut::editorTrackMediaPresenceCore(snapshot, track.id);
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
        ImGui::BeginDisabled(!trackPresence.hasVisual);
        int visualMode = std::clamp(trackState.visualMode, 0, 2);
        ImGui::SetNextItemWidth(104.0f);
        if (ImGui::Combo("##visualMode", &visualMode, kTrackVisualModeLabels,
                         IM_ARRAYSIZE(kTrackVisualModeLabels))) {
            trackState.visualMode = visualMode;
            applyCommand(shellState, trackState);
        }
        ImGui::EndDisabled();
        if (!trackPresence.hasVisual &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("No visual clips on this track");
        }
        ImGui::TableNextColumn();
        ImGui::BeginDisabled(!trackPresence.hasVisual);
        bool gradingPreviewEnabled = trackState.gradingPreviewEnabled;
        if (ImGui::Checkbox("##gradingPreviewEnabled", &gradingPreviewEnabled)) {
            trackState.gradingPreviewEnabled = gradingPreviewEnabled;
            applyCommand(shellState, trackState);
        }
        ImGui::EndDisabled();
        if (!trackPresence.hasVisual &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("No visual clips on this track");
        }
        const bool audioControlsDisabled =
            generatedChildTrack || !trackPresence.hasAudio;
        ImGui::BeginDisabled(audioControlsDisabled);
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
        if (audioControlsDisabled &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                generatedChildTrack
                    ? "Generated child lanes do not expose audio controls"
                    : "No audio clips on this track");
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void startAiAccountRefresh(ShellState* shellState);
void startAiTokenRefresh(ShellState* shellState);

void appendAiActivity(
    ShellState* shellState,
    std::string phase,
    std::string summary)
{
    if (!shellState) return;
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    std::array<char, 16> timeBuffer{};
    std::strftime(
        timeBuffer.data(), timeBuffer.size(),
        "%H:%M:%S", &local);
    shellState->aiActivityEntries.push_back(
        AiActivityEntry{
            timeBuffer.data(),
            std::move(phase),
            std::move(summary)});
    if (shellState->aiActivityEntries.size() > 200) {
        shellState->aiActivityEntries.erase(
            shellState->aiActivityEntries.begin(),
            shellState->aiActivityEntries.begin() +
                static_cast<std::ptrdiff_t>(
                    shellState->aiActivityEntries.size() - 200));
    }
}

void drawAiProfileAvatar(
    const ShellState& shellState,
    const std::string& identity)
{
    constexpr float size = 40.0f;
    const ImVec2 minimum = ImGui::GetCursorScreenPos();
    const ImVec2 maximum{
        minimum.x + size, minimum.y + size};
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const bool subscribed =
        shellState.aiAccount.usage.hasSubscription;
    const bool entitled =
        shellState.aiAccount.entitlements.entitled;
    const ImU32 ringColor = subscribed
        ? IM_COL32(237, 196, 78, 255)
        : entitled
            ? IM_COL32(76, 190, 112, 255)
            : IM_COL32(108, 116, 128, 255);
    drawList->AddCircleFilled(
        ImVec2(
            (minimum.x + maximum.x) * 0.5f,
            (minimum.y + maximum.y) * 0.5f),
        size * 0.5f,
        ringColor,
        32);
    const ImVec2 imageMinimum{
        minimum.x + 3.0f, minimum.y + 3.0f};
    const ImVec2 imageMaximum{
        maximum.x - 3.0f, maximum.y - 3.0f};
    if (shellState.aiAvatarTextureId != 0 &&
        shellState.aiAvatarSize.valid()) {
        ImVec2 uvMinimum{0.0f, 0.0f};
        ImVec2 uvMaximum{1.0f, 1.0f};
        if (shellState.aiAvatarSize.width >
            shellState.aiAvatarSize.height) {
            const float inset =
                (shellState.aiAvatarSize.width -
                 shellState.aiAvatarSize.height) /
                (2.0f * shellState.aiAvatarSize.width);
            uvMinimum.x = inset;
            uvMaximum.x = 1.0f - inset;
        } else if (shellState.aiAvatarSize.height >
                   shellState.aiAvatarSize.width) {
            const float inset =
                (shellState.aiAvatarSize.height -
                 shellState.aiAvatarSize.width) /
                (2.0f * shellState.aiAvatarSize.height);
            uvMinimum.y = inset;
            uvMaximum.y = 1.0f - inset;
        }
        drawList->AddImageRounded(
            shellState.aiAvatarTextureId,
            imageMinimum,
            imageMaximum,
            uvMinimum,
            uvMaximum,
            IM_COL32_WHITE,
            (size - 6.0f) * 0.5f);
    } else {
        drawList->AddCircleFilled(
            ImVec2(
                (imageMinimum.x + imageMaximum.x) * 0.5f,
                (imageMinimum.y + imageMaximum.y) * 0.5f),
            (size - 6.0f) * 0.5f,
            IM_COL32(33, 42, 56, 255),
            32);
        std::string initials;
        for (unsigned char character : identity) {
            if (character == '@' || initials.size() == 2) break;
            if (std::isalnum(character)) {
                initials.push_back(static_cast<char>(
                    std::toupper(character)));
            }
        }
        if (initials.empty()) initials = "JC";
        const ImVec2 textSize =
            ImGui::CalcTextSize(initials.c_str());
        drawList->AddText(
            ImVec2(
                minimum.x + (size - textSize.x) * 0.5f,
                minimum.y + (size - textSize.y) * 0.5f),
            IM_COL32(245, 247, 250, 255),
            initials.c_str());
    }
    ImGui::Dummy(ImVec2(size, size));
    if (ImGui::IsItemHovered() &&
        !shellState.aiAvatarError.empty()) {
        ImGui::SetTooltip(
            "%s", shellState.aiAvatarError.c_str());
    }
}

void pollAiOperations(ShellState* shellState)
{
    if (shellState->aiCheckoutRunning &&
        shellState->aiCheckoutFuture.valid() &&
        shellState->aiCheckoutFuture.wait_for(
            std::chrono::seconds(0)) == std::future_status::ready) {
        const jcut::ai::CheckoutLaunchCore checkout =
            shellState->aiCheckoutFuture.get();
        shellState->aiCheckoutRunning = false;
        shellState->statusMessage = checkout.ok
            ? "Subscription checkout opened in the browser."
            : "Checkout failed: " + checkout.error.message;
        appendAiActivity(
            shellState,
            checkout.ok ? "Checkout" : "Checkout error",
            shellState->statusMessage);
    }
    if (shellState->aiBrowserLoginRunning &&
        shellState->aiBrowserLoginFuture.valid() &&
        shellState->aiBrowserLoginFuture.wait_for(
            std::chrono::seconds(0)) == std::future_status::ready) {
        const jcut::ai::BrowserLoginCore login =
            shellState->aiBrowserLoginFuture.get();
        shellState->aiBrowserLoginRunning = false;
        if (login.ok) {
            shellState->aiSessionToken = login.accessToken;
            shellState->aiRefreshToken = login.refreshToken;
            shellState->aiUserId = login.userId;
            const jcut::ai::CredentialStoreResultCore stored =
                jcut::ai::storeCredentialsCore(
                    jcut::ai::StoredCredentialsCore{
                        shellState->aiSessionToken,
                        shellState->aiRefreshToken,
                        shellState->aiUserId});
            shellState->aiCredentialStatus = stored.ok
                ? "Browser login completed and saved."
                : "Browser login completed for this session; save failed: " +
                    stored.error;
            shellState->statusMessage =
                shellState->aiCredentialStatus;
            startAiAccountRefresh(shellState);
        } else {
            shellState->aiCredentialStatus =
                "Browser login failed: " + login.error.message;
            shellState->statusMessage =
                shellState->aiCredentialStatus;
        }
        appendAiActivity(
            shellState,
            login.ok ? "Login" : "Login error",
            shellState->aiCredentialStatus);
    }
    if (shellState->aiTokenRefreshRunning &&
        shellState->aiTokenRefreshFuture.valid() &&
        shellState->aiTokenRefreshFuture.wait_for(
            std::chrono::seconds(0)) == std::future_status::ready) {
        const jcut::ai::RefreshedSessionCore refreshed =
            shellState->aiTokenRefreshFuture.get();
        shellState->aiTokenRefreshRunning = false;
        if (refreshed.ok) {
            shellState->aiSessionToken = refreshed.accessToken;
            shellState->aiRefreshToken = refreshed.refreshToken;
            if (!refreshed.userId.empty()) {
                shellState->aiUserId = refreshed.userId;
            }
            const jcut::ai::CredentialStoreResultCore stored =
                jcut::ai::storeCredentialsCore(
                    jcut::ai::StoredCredentialsCore{
                        shellState->aiSessionToken,
                        shellState->aiRefreshToken,
                        shellState->aiUserId});
            shellState->aiCredentialStatus = stored.ok
                ? "Login token refreshed and saved."
                : "Token refreshed for this session; save failed: " +
                    stored.error;
            shellState->statusMessage =
                shellState->aiCredentialStatus;
            startAiAccountRefresh(shellState);
        } else {
            shellState->aiCredentialStatus =
                "Token refresh failed: " + refreshed.error.message;
            shellState->statusMessage =
                shellState->aiCredentialStatus;
        }
        appendAiActivity(
            shellState,
            refreshed.ok ? "Token refresh" : "Token error",
            shellState->aiCredentialStatus);
    }
    if (shellState->aiAccountRefreshRunning &&
        shellState->aiAccountFuture.valid() &&
        shellState->aiAccountFuture.wait_for(std::chrono::seconds(0)) ==
            std::future_status::ready) {
        shellState->aiAccount = shellState->aiAccountFuture.get();
        shellState->aiAccountRefreshRunning = false;
        if (!shellState->aiAccount.ok &&
            (shellState->aiAccount.error.httpStatus == 401 ||
             shellState->aiAccount.error.httpStatus == 403) &&
            !shellState->aiRefreshToken.empty()) {
            startAiTokenRefresh(shellState);
        }
        if (shellState->aiAccount.ok) {
            const jcut::ai::EntitlementsCore& entitlements =
                shellState->aiAccount.entitlements;
            if (!entitlements.models.empty() &&
                std::find(entitlements.models.begin(),
                          entitlements.models.end(),
                          shellState->aiSelectedModel) ==
                    entitlements.models.end()) {
                shellState->aiSelectedModel = entitlements.models.front();
                setLegacyStateOverride(
                    shellState, "aiSelectedModel",
                    shellState->aiSelectedModel);
            }
            if (entitlements.projectBudget > 0) {
                shellState->aiUsageBudgetCap =
                    entitlements.projectBudget;
                setLegacyStateOverride(
                    shellState, "aiUsageBudgetCap",
                    shellState->aiUsageBudgetCap);
            }
            if (entitlements.timeoutMs > 0) {
                shellState->aiRequestTimeoutMs =
                    entitlements.timeoutMs;
            }
            shellState->aiRequestRetries =
                std::clamp(entitlements.retries, 0, 3);
        }
        shellState->statusMessage = shellState->aiAccount.status;
        appendAiActivity(
            shellState,
            shellState->aiAccount.ok
                ? "Access refresh"
                : "Access error",
            shellState->aiAccount.status);
    }
    if (shellState->aiTaskRunning &&
        shellState->aiTaskFuture.valid() &&
        shellState->aiTaskFuture.wait_for(std::chrono::seconds(0)) ==
            std::future_status::ready) {
        jcut::ai::TaskResponseCore response =
            shellState->aiTaskFuture.get();
        shellState->aiTaskRunning = false;
        if (response.ok) {
            shellState->aiUsageRequests += 1;
            setLegacyStateOverride(
                shellState, "aiUsageRequests",
                shellState->aiUsageRequests);
            if (shellState->aiTaskPurpose ==
                AiTaskPurpose::CloudSpeakerMining) {
                TranscriptInspectorCache& cache =
                    shellState->transcriptCache;
                std::string parseError;
                std::vector<jcut::TranscriptMiningProposal> proposals;
                if (cache.sourceKey ==
                        shellState->aiTaskTranscriptSourceKey &&
                    cache.session.activeDocument) {
                    try {
                        proposals =
                            jcut::parseCloudSpeakerMiningResponse(
                                cache.session.activeDocument->root(),
                                nlohmann::json::parse(
                                    response.responseJson),
                                &parseError);
                    } catch (const nlohmann::json::exception& exception) {
                        parseError = exception.what();
                    }
                } else {
                    parseError =
                        "Transcript changed while cloud mining was running.";
                }
                cache.miningProposalLabel =
                    "Cloud speaker-profile candidates";
                cache.miningProposals = std::move(proposals);
                cache.miningProposalSelected.assign(
                    cache.miningProposals.size(), 1);
                shellState->statusMessage =
                    cache.miningProposals.empty()
                    ? parseError
                    : "Cloud speaker-profile proposals are ready for review.";
            } else {
                shellState->aiChatMessages.push_back(AiChatMessage{
                    "Assistant",
                    response.text.empty()
                        ? std::string("No text response returned.")
                        : std::move(response.text)});
                shellState->statusMessage = "AI response received";
            }
            appendAiActivity(
                shellState,
                shellState->aiTaskPurpose ==
                        AiTaskPurpose::CloudSpeakerMining
                    ? "Speaker mining"
                    : "Chat",
                shellState->statusMessage);
        } else {
            shellState->aiUsageFailures += 1;
            setLegacyStateOverride(
                shellState, "aiUsageFailures",
                shellState->aiUsageFailures);
            const std::string failure =
                response.error.message.empty()
                ? std::string("AI request failed")
                : std::move(response.error.message);
            if (shellState->aiTaskPurpose ==
                AiTaskPurpose::Chat) {
                shellState->aiChatMessages.push_back(
                    AiChatMessage{"Error", failure});
            }
            shellState->statusMessage = failure;
            appendAiActivity(
                shellState, "AI request error", failure);
        }
    }
}

void startAiCheckout(ShellState* shellState)
{
    if (!shellState || shellState->aiCheckoutRunning ||
        shellState->aiSessionToken.empty()) {
        return;
    }
    jcut::ai::GatewayConfigCore config;
    config.baseUrl = shellState->aiGatewayBaseUrl;
    config.timeoutMs = shellState->aiRequestTimeoutMs;
    const std::string token = shellState->aiSessionToken;
    std::vector<std::string> slugs;
    if (const char* configured =
            std::getenv("JCUT_AI_SUBSCRIPTION_SLUG");
        configured && *configured) {
        slugs.emplace_back(configured);
    }
    for (const char* fallback :
         {"jsynth-pro-subscription",
          "jcut-ai-subscription",
          "ai-platform"}) {
        if (std::find(slugs.begin(), slugs.end(), fallback) ==
            slugs.end()) {
            slugs.emplace_back(fallback);
        }
    }
    shellState->aiCheckoutRunning = true;
    shellState->statusMessage =
        "Starting subscription checkout...";
    appendAiActivity(
        shellState, "Checkout",
        shellState->statusMessage);
    shellState->aiCheckoutFuture = std::async(
        std::launch::async,
        [config = std::move(config),
         token,
         slugs = std::move(slugs)] {
            return jcut::ai::launchSubscriptionCheckoutCore(
                config, token, slugs);
        });
}

void startAiBrowserLogin(ShellState* shellState)
{
    if (!shellState || shellState->aiBrowserLoginRunning) return;
    jcut::ai::GatewayConfigCore config;
    config.baseUrl = shellState->aiGatewayBaseUrl;
    config.timeoutMs = shellState->aiRequestTimeoutMs;
    shellState->aiBrowserLoginCancelRequested.store(false);
    shellState->aiBrowserLoginRunning = true;
    shellState->aiCredentialStatus =
        "Browser login started; complete sign-in in your browser.";
    appendAiActivity(
        shellState, "Login",
        shellState->aiCredentialStatus);
    shellState->aiBrowserLoginFuture = std::async(
        std::launch::async,
        [config = std::move(config), shellState] {
            return jcut::ai::runSupabaseBrowserLoginCore(
                config,
                "google",
                180000,
                &shellState->aiBrowserLoginCancelRequested);
        });
}

void startAiTokenRefresh(ShellState* shellState)
{
    if (!shellState || shellState->aiTokenRefreshRunning ||
        shellState->aiRefreshToken.empty()) {
        return;
    }
    jcut::ai::GatewayConfigCore config;
    config.baseUrl = shellState->aiGatewayBaseUrl;
    config.timeoutMs = shellState->aiRequestTimeoutMs;
    const std::string refreshToken = shellState->aiRefreshToken;
    shellState->aiTokenRefreshRunning = true;
    shellState->aiCredentialStatus = "Refreshing login token...";
    appendAiActivity(
        shellState, "Token refresh",
        shellState->aiCredentialStatus);
    shellState->aiTokenRefreshFuture = std::async(
        std::launch::async,
        [config = std::move(config), refreshToken] {
            return jcut::ai::refreshSupabaseSessionCore(
                config, refreshToken);
        });
}

void startAiAccountRefresh(ShellState* shellState)
{
    if (!shellState || shellState->aiAccountRefreshRunning) return;
    jcut::ai::GatewayConfigCore config;
    config.baseUrl = shellState->aiGatewayBaseUrl;
    config.timeoutMs = shellState->aiRequestTimeoutMs;
    const std::string token = shellState->aiSessionToken;
    shellState->aiAccountRefreshRunning = true;
    shellState->aiAccount.status = "Refreshing account access...";
    appendAiActivity(
        shellState, "Access refresh",
        shellState->aiAccount.status);
    shellState->aiAccountFuture = std::async(
        std::launch::async,
        [config = std::move(config), token] {
            return jcut::ai::refreshAccountCore(config, token);
        });
}

nlohmann::json buildAiProjectContextCore(
    const jcut::EditorDocumentCore& snapshot)
{
    nlohmann::json clips = nlohmann::json::array();
    int selectedClip = 0;
    for (const jcut::EditorClip& clip : snapshot.clips) {
        if (clip.selected) selectedClip = clip.id;
        clips.push_back(nlohmann::json{
            {"id", clip.id},
            {"label", clip.label},
            {"file_path", clip.sourcePath},
            {"track_id", clip.trackId},
            {"start_frame", clip.startFrame},
            {"duration_frames", clip.durationFrames},
            {"has_audio", clip.hasAudio},
            {"media_type", clip.mediaKind},
        });
    }
    return nlohmann::json{
        {"current_frame", snapshot.transport.currentFrame},
        {"selected_clip_id", selectedClip},
        {"clips", std::move(clips)},
    };
}

void startAiChatRequest(ShellState* shellState,
                        const jcut::EditorDocumentCore& snapshot)
{
    if (!shellState || shellState->aiTaskRunning) return;
    const std::string prompt = shellState->aiChatPrompt;
    if (prompt.empty()) return;
    if (!shellState->featureAiPanel) {
        shellState->statusMessage =
            "AI disabled: feature_ai_panel=false";
        return;
    }
    if (!shellState->aiAccount.aiEnabled) {
        shellState->statusMessage =
            "Refresh account access before sending AI requests";
        return;
    }
    if (shellState->aiUsageRequests >=
        shellState->aiUsageBudgetCap) {
        shellState->statusMessage =
            "AI project request budget is exhausted";
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    std::erase_if(
        shellState->aiRecentRequestTimes,
        [now](const auto& requestTime) {
            return now - requestTime >= std::chrono::minutes(1);
        });
    const int requestsPerMinute = std::max(
        1,
        shellState->aiAccount.entitlements.requestsPerMinute);
    if (static_cast<int>(
            shellState->aiRecentRequestTimes.size()) >=
        requestsPerMinute) {
        shellState->statusMessage =
            "AI rate limit reached (" +
            std::to_string(requestsPerMinute) +
            " requests/min)";
        return;
    }

    shellState->aiChatMessages.push_back({"You", prompt});
    shellState->aiChatPrompt.clear();
    while (shellState->aiChatMessages.size() > 30) {
        shellState->aiChatMessages.erase(
            shellState->aiChatMessages.begin());
    }
    std::string conversation;
    for (const AiChatMessage& message :
         shellState->aiChatMessages) {
        conversation += "[" + message.role + "]\n" +
            message.content + "\n\n";
    }
    nlohmann::json payload{
        {"task", "chat_agent"},
        {"instructions",
         "You are JCut Agent. Use the provided conversation and project "
         "context to answer accurately. Give concise, actionable responses."},
        {"transcript_text", std::move(conversation)},
    };
    const nlohmann::json context =
        buildAiProjectContextCore(snapshot);
    jcut::ai::GatewayConfigCore config;
    config.baseUrl = shellState->aiGatewayBaseUrl;
    config.timeoutMs = shellState->aiRequestTimeoutMs;
    const std::string token = shellState->aiSessionToken;
    std::vector<std::string> models{
        shellState->aiSelectedModel.empty()
            ? std::string("deepseek-chat")
            : shellState->aiSelectedModel};
    for (const std::string& fallback :
         shellState->aiAccount.entitlements.fallbackOrder) {
        if (!fallback.empty() &&
            std::find(models.begin(), models.end(), fallback) ==
                models.end()) {
            models.push_back(fallback);
        }
    }
    const int retries = shellState->aiRequestRetries;
    shellState->aiTaskRunning = true;
    shellState->aiTaskPurpose = AiTaskPurpose::Chat;
    shellState->aiTaskTranscriptSourceKey.clear();
    shellState->aiRecentRequestTimes.push_back(now);
    shellState->statusMessage = "Submitting AI chat request...";
    appendAiActivity(
        shellState, "Chat",
        shellState->statusMessage);
    shellState->aiTaskFuture = std::async(
        std::launch::async,
        [config = std::move(config),
         token,
         models = std::move(models),
         retries,
         payload = std::move(payload),
         context]() {
            jcut::ai::TaskResponseCore last;
            for (const std::string& model : models) {
                for (int attempt = 0; attempt <= retries; ++attempt) {
                    last = jcut::ai::submitTaskCore(
                        config, token, "chat", model, payload, context);
                    if (last.ok) return last;
                    if (last.error.httpStatus == 401 ||
                        last.error.httpStatus == 403) {
                        return last;
                    }
                }
            }
            return last;
        });
}

void startCloudSpeakerMining(
    ShellState* shellState,
    const nlohmann::json& transcriptRoot,
    const std::string& transcriptSourceKey,
    const jcut::EditorDocumentCore& snapshot)
{
    if (!shellState || shellState->aiTaskRunning ||
        !shellState->featureAiSpeakerCleanup ||
        !shellState->aiAccount.aiEnabled) {
        if (shellState) {
            shellState->statusMessage =
                "Cloud speaker mining requires enabled AI access.";
        }
        return;
    }
    if (shellState->aiUsageRequests >=
        shellState->aiUsageBudgetCap) {
        shellState->statusMessage =
            "AI project request budget is exhausted";
        return;
    }
    jcut::ai::GatewayConfigCore config;
    config.baseUrl = shellState->aiGatewayBaseUrl;
    config.timeoutMs = shellState->aiRequestTimeoutMs;
    const std::string token = shellState->aiSessionToken;
    const std::string model = shellState->aiSelectedModel;
    const nlohmann::json payload =
        jcut::buildCloudSpeakerMiningPayload(transcriptRoot);
    const nlohmann::json context =
        buildAiProjectContextCore(snapshot);
    shellState->aiTaskRunning = true;
    shellState->aiTaskPurpose =
        AiTaskPurpose::CloudSpeakerMining;
    shellState->aiTaskTranscriptSourceKey =
        transcriptSourceKey;
    shellState->statusMessage =
        "Mining cloud speaker profiles...";
    appendAiActivity(
        shellState, "Speaker mining",
        shellState->statusMessage);
    shellState->aiTaskFuture = std::async(
        std::launch::async,
        [config = std::move(config),
         token,
         model,
         payload,
         context] {
            return jcut::ai::submitTaskCore(
                config,
                token,
                "mine_transcript_speakers",
                model,
                payload,
                context);
        });
}

#include "jcut_imgui_inspector_tabs_visual.h"
#include "jcut_imgui_inspector_tabs_edit.h"
#include "jcut_imgui_inspector_tab_transcript.h"
#include "jcut_imgui_inspector_tab_speakers.h"
#include "jcut_imgui_inspector_tabs_project.h"
#include "jcut_imgui_inspector_tabs_output.h"

void drawInspectorPanel(ShellState* shellState, const jcut::EditorDocumentCore& snapshot)
{
    pollAiOperations(shellState);
    const std::string requestedInspectorTab =
        std::exchange(shellState->requestedInspectorTab, {});
    const bool focusOutput = std::exchange(shellState->focusInspectorOutputRequested, false);
    const bool focusProjects = std::exchange(shellState->focusInspectorProjectsRequested, false);
    const ShellLayout layout = computeShellLayout();
    const ImGuiCond layoutCondition = shellState->resetLayoutRequested
        ? ImGuiCond_Always
        : ImGuiCond_FirstUseEver;
    ImGui::SetNextWindowPos(layout.inspector.pos, layoutCondition);
    ImGui::SetNextWindowSize(layout.inspector.size, layoutCondition);
    if (focusOutput || focusProjects || !requestedInspectorTab.empty()) {
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
        drawInspectorTab00(shellState, snapshot, currentClip, currentTrack, currentClipLocalFrame, currentClipLastFrame, fadeEndFrame, requestedInspectorTab, focusOutput, focusProjects);
        drawInspectorTab01(shellState, snapshot, currentClip, currentTrack, currentClipLocalFrame, currentClipLastFrame, fadeEndFrame, requestedInspectorTab, focusOutput, focusProjects);
        drawInspectorTab02(shellState, snapshot, currentClip, currentTrack, currentClipLocalFrame, currentClipLastFrame, fadeEndFrame, requestedInspectorTab, focusOutput, focusProjects);
        drawInspectorTab03(shellState, snapshot, currentClip, currentTrack, currentClipLocalFrame, currentClipLastFrame, fadeEndFrame, requestedInspectorTab, focusOutput, focusProjects);
        drawInspectorTab04(shellState, snapshot, currentClip, currentTrack, currentClipLocalFrame, currentClipLastFrame, fadeEndFrame, requestedInspectorTab, focusOutput, focusProjects);
        drawInspectorTab05(shellState, snapshot, currentClip, currentTrack, currentClipLocalFrame, currentClipLastFrame, fadeEndFrame, requestedInspectorTab, focusOutput, focusProjects);
        drawInspectorTab06(shellState, snapshot, currentClip, currentTrack, currentClipLocalFrame, currentClipLastFrame, fadeEndFrame, requestedInspectorTab, focusOutput, focusProjects);
        drawInspectorTab07(shellState, snapshot, currentClip, currentTrack, currentClipLocalFrame, currentClipLastFrame, fadeEndFrame, requestedInspectorTab, focusOutput, focusProjects);
        drawInspectorTab08(shellState, snapshot, currentClip, currentTrack, currentClipLocalFrame, currentClipLastFrame, fadeEndFrame, requestedInspectorTab, focusOutput, focusProjects);
        drawInspectorTab09(shellState, snapshot, currentClip, currentTrack, currentClipLocalFrame, currentClipLastFrame, fadeEndFrame, requestedInspectorTab, focusOutput, focusProjects);
        drawInspectorTab10(shellState, snapshot, currentClip, currentTrack, currentClipLocalFrame, currentClipLastFrame, fadeEndFrame, requestedInspectorTab, focusOutput, focusProjects);
        drawInspectorTab11(shellState, snapshot, currentClip, currentTrack, currentClipLocalFrame, currentClipLastFrame, fadeEndFrame, requestedInspectorTab, focusOutput, focusProjects);
        drawInspectorTab12(shellState, snapshot, currentClip, currentTrack, currentClipLocalFrame, currentClipLastFrame, fadeEndFrame, requestedInspectorTab, focusOutput, focusProjects);
        drawInspectorTab13(shellState, snapshot, currentClip, currentTrack, currentClipLocalFrame, currentClipLastFrame, fadeEndFrame, requestedInspectorTab, focusOutput, focusProjects);
        drawInspectorTab14(shellState, snapshot, currentClip, currentTrack, currentClipLocalFrame, currentClipLastFrame, fadeEndFrame, requestedInspectorTab, focusOutput, focusProjects);
        drawInspectorTab15(shellState, snapshot, currentClip, currentTrack, currentClipLocalFrame, currentClipLastFrame, fadeEndFrame, requestedInspectorTab, focusOutput, focusProjects);
        drawInspectorTab16(shellState, snapshot, currentClip, currentTrack, currentClipLocalFrame, currentClipLastFrame, fadeEndFrame, requestedInspectorTab, focusOutput, focusProjects);
        drawInspectorTab17(shellState, snapshot, currentClip, currentTrack, currentClipLocalFrame, currentClipLastFrame, fadeEndFrame, requestedInspectorTab, focusOutput, focusProjects);
        drawInspectorTab18(shellState, snapshot, currentClip, currentTrack, currentClipLocalFrame, currentClipLastFrame, fadeEndFrame, requestedInspectorTab, focusOutput, focusProjects);
        drawInspectorTab19(shellState, snapshot, currentClip, currentTrack, currentClipLocalFrame, currentClipLastFrame, fadeEndFrame, requestedInspectorTab, focusOutput, focusProjects);
        drawInspectorTab20(shellState, snapshot, currentClip, currentTrack, currentClipLocalFrame, currentClipLastFrame, fadeEndFrame, requestedInspectorTab, focusOutput, focusProjects);
        drawInspectorTab21(shellState, snapshot, currentClip, currentTrack, currentClipLocalFrame, currentClipLastFrame, fadeEndFrame, requestedInspectorTab, focusOutput, focusProjects);
        drawInspectorTab22(shellState, snapshot, currentClip, currentTrack, currentClipLocalFrame, currentClipLastFrame, fadeEndFrame, requestedInspectorTab, focusOutput, focusProjects);
        drawInspectorTab23(shellState, snapshot, currentClip, currentTrack, currentClipLocalFrame, currentClipLastFrame, fadeEndFrame, requestedInspectorTab, focusOutput, focusProjects);
        drawInspectorTab24(shellState, snapshot, currentClip, currentTrack, currentClipLocalFrame, currentClipLastFrame, fadeEndFrame, requestedInspectorTab, focusOutput, focusProjects);
        drawInspectorTab25(shellState, snapshot, currentClip, currentTrack, currentClipLocalFrame, currentClipLastFrame, fadeEndFrame, requestedInspectorTab, focusOutput, focusProjects);
        drawInspectorTab26(shellState, snapshot, currentClip, currentTrack, currentClipLocalFrame, currentClipLastFrame, fadeEndFrame, requestedInspectorTab, focusOutput, focusProjects);
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

int runJcutImGuiApplication(int argc, char** argv)
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
    shellState.audioRuntime.setOutputDeviceName(
        shellState.audioOutputDeviceName);
    shellState.audioRuntime.refreshOutputDevices();
    shellState.layoutIniPath = pathString(
        fs::path(shellState.preferencesPath).parent_path() / "imgui_layout.ini");
    std::string gpuRendererError;
    if (shellState.gpuRenderer.initialize(
            fs::absolute(argv[0]).string(),
            &gpuRendererError)) {
        shellState.statusMessage =
            shellState.gpuRenderer.status();
    } else {
        shellState.statusMessage = gpuRendererError;
    }

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
    const std::uint16_t controlPort = jcut::runtimeControlPortFromEnvironment(40131);
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
        refreshBiRefNetLivePreviewTexture(
            &shellState, &vulkanShell);
        refreshMediaThumbnailTexture(
            &shellState, &vulkanShell);
        refreshAiProfileAvatarTexture(
            &shellState, &vulkanShell);
        refreshFaceReferenceTexture(
            &shellState, &vulkanShell);
        refreshSectionAvatarTexture(
            &shellState, &vulkanShell);

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
        ++shellState.uiFrameCounter;
        pollAutoOpposeJob(&shellState);
        pollTranscriptionJob(&shellState);
        pollBiRefNetJob(&shellState);

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

    shellState.aiBrowserLoginCancelRequested.store(true);
    if (shellState.aiBrowserLoginFuture.valid()) {
        shellState.aiBrowserLoginFuture.wait();
    }
    if (shellState.mediaThumbnailFuture.valid()) {
        shellState.mediaThumbnailFuture.wait();
    }
    if (shellState.aiAvatarFuture.valid()) {
        shellState.aiAvatarFuture.wait();
    }
    if (shellState.faceReferenceFuture.valid()) {
        shellState.faceReferenceFuture.wait();
    }
    if (shellState.sectionAvatarFuture.valid()) {
        shellState.sectionAvatarFuture.wait();
    }
    if (!shellState.aiAvatarCachePath.empty()) {
        std::error_code ignored;
        fs::remove(shellState.aiAvatarCachePath, ignored);
    }
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
    shellState.gpuRenderer.shutdown();
    shellState.controlServer.stop();
    vulkanShell.shutdown();
    shellState.audioRuntime.shutdown();
    ImGui::SaveIniSettingsToDisk(shellState.layoutIniPath.c_str());
    ImGui::DestroyContext();
    platform.shutdown();
    return 0;
}
