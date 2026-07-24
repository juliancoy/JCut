#include "editor_runtime.h"
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
    std::string lastSavedSnapshotJson;
    std::string statusMessage;
    jcut::ImGuiAudioRuntime audioRuntime;
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
        shellState->previewDecoderPolicy = shellState->decoderPolicy;
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
    shellState->exportQueue.clear();
    shellState->exportQueueCurrent = 0;
    shellState->exportQueueTotal = 1;
    shellState->exportQueueCompleted = 0;
    shellState->exportQueueFailed = 0;
    shellState->exportQueueLabel.clear();
    shellState->exportDecoderPolicy = shellState->decoderPolicy;
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

bool requestExportBatch(
    ShellState* shellState,
    std::vector<ShellState::QueuedExport> queue)
{
    if (queue.empty()) return false;
    std::lock_guard<std::mutex> lock(
        shellState->exportMutex);
    if (shellState->exportRunning ||
        shellState->exportRequested) {
        return false;
    }
    shellState->exportQueue = std::move(queue);
    shellState->exportRootDirectory =
        shellState->mediaRootDirectory.empty()
        ? shellState->projectRootPath
        : shellState->mediaRootDirectory;
    shellState->exportRequested = true;
    shellState->exportCancelRequested = false;
    shellState->exportHasProgress = false;
    shellState->exportProgress = {};
    shellState->exportResult = {};
    shellState->exportQueueCurrent = 0;
    shellState->exportQueueTotal =
        shellState->exportQueue.size();
    shellState->exportQueueCompleted = 0;
    shellState->exportQueueFailed = 0;
    shellState->exportQueueLabel.clear();
    ++shellState->exportRequestGeneration;
    shellState->exportCondition.notify_one();
    return true;
}

std::size_t requestSpeakerSectionExportBatch(
    ShellState* shellState,
    const jcut::EditorDocumentCore& snapshot,
    const jcut::EditorClip& clip,
    const nlohmann::json& transcriptRoot,
    const std::vector<jcut::SpeakerSectionCore>& sections,
    std::size_t* skippedOut = nullptr)
{
    if (skippedOut) *skippedOut = 0;
    const fs::path configuredPath(
        shellState->exportOutputPath.data());
    if (configuredPath.empty()) return 0;
    fs::path outputDirectory =
        configuredPath.parent_path();
    if (outputDirectory.empty()) {
        outputDirectory = fs::current_path();
    }
    const std::string format =
        snapshot.exportRequest.outputFormat.empty()
        ? "mp4"
        : snapshot.exportRequest.outputFormat;
    const std::string extension =
        format == "mov_mjpeg" ? "mov" : format;
    const std::string clipIdentity =
        clip.persistentId.empty()
        ? std::to_string(clip.id)
        : clip.persistentId;

    std::vector<jcut::SpeakerSectionExportCore> candidates;
    candidates.reserve(sections.size());
    for (std::size_t index = 0;
         index < sections.size();
         ++index) {
        const auto& section = sections[index];
        jcut::SpeakerSectionExportCore candidate;
        candidate.speakerId = section.speakerId;
        candidate.speakerDisplayName =
            section.displayLabel;
        candidate.sourceStartFrame =
            section.startFrame;
        candidate.sourceEndFrame =
            section.endFrame;
        candidate.wordCount = section.wordCount;
        candidate.sectionOrdinal =
            static_cast<int>(index + 1);
        for (const auto& assignment :
             jcut::transcriptSpeakerTrackAssignmentsAtFrame(
                 transcriptRoot,
                 clipIdentity,
                 section.speakerId,
                 section.startFrame)) {
            candidate.trackIds.push_back(
                assignment.trackId);
        }
        candidates.push_back(std::move(candidate));
    }
    candidates =
        jcut::coalescedSpeakerSectionExports(
            candidates);

    std::vector<ShellState::QueuedExport> queue;
    std::set<std::string> reservedPaths;
    std::size_t skipped = 0;
    for (const auto& section : candidates) {
        const auto ranges =
            jcut::editorTimelineRangesForTranscriptSection(
                snapshot,
                clip,
                section.sourceStartFrame,
                section.sourceEndFrame);
        if (ranges.empty()) {
            ++skipped;
            continue;
        }
        const fs::path outputPath =
            outputDirectory /
            (jcut::sanitizedSpeakerSectionExportBase(
                 section) +
             jcut::speakerSectionExportSpeedSuffix(
                 snapshot.exportRequest.playbackSpeed) +
             "." + extension);
        const std::string normalizedOutput =
            pathString(outputPath);
        std::error_code existsError;
        if (reservedPaths.contains(
                normalizedOutput) ||
            fs::exists(outputPath, existsError)) {
            ++skipped;
            continue;
        }
        reservedPaths.insert(normalizedOutput);
        jcut::EditorDocumentCore document =
            snapshot;
        document.exportRanges = ranges;
        document.exportRequest.exportStartFrame =
            ranges.front().startFrame;
        document.exportRequest.exportEndFrame =
            ranges.back().endFrame;
        document.exportRequest.exportRangeCount =
            ranges.size();
        document.exportRequest.outputPath =
            normalizedOutput;
        queue.push_back({
            std::move(document),
            jcut::speakerSectionExportTitle(section),
        });
    }
    if (skippedOut) *skippedOut = skipped;
    const std::size_t count = queue.size();
    return requestExportBatch(
        shellState, std::move(queue))
        ? count
        : 0;
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

fs::path resolvedClipSourcePath(const ShellState& shellState,
                                const jcut::EditorClip& clip)
{
    const fs::path mediaRoot = !shellState.mediaRootDirectory.empty()
        ? fs::path(shellState.mediaRootDirectory)
        : fs::path(shellState.projectRootPath);
    fs::path path(clip.sourcePath);
    if (path.is_relative() && !mediaRoot.empty()) {
        path = mediaRoot / path;
    }
    return path.lexically_normal();
}

fs::path resolvedClipProxyPath(const ShellState& shellState,
                               const jcut::EditorClip& clip)
{
    const fs::path mediaRoot = !shellState.mediaRootDirectory.empty()
        ? fs::path(shellState.mediaRootDirectory)
        : fs::path(shellState.projectRootPath);
    fs::path path(clip.proxyPath);
    if (path.is_relative() && !mediaRoot.empty()) {
        path = mediaRoot / path;
    }
    return path.lexically_normal();
}

jcut::EditorDocumentCore runtimeSnapshot(ShellState* shellState);
void beginRuntimeHistoryTransaction(ShellState* shellState);
void endRuntimeHistoryTransaction(ShellState* shellState);

void startAutoOpposeJob(ShellState* shellState,
                        const jcut::EditorDocumentCore& snapshot,
                        const jcut::EditorClip& clip)
{
    if (shellState->autoOpposeRunning) {
        return;
    }
    if (!jcut::editorClipHasVisualsCore(clip) ||
        clip.mediaKind == "title" || clip.sourcePath.empty()) {
        shellState->statusMessage =
            "Auto Oppose requires a selected decoded visual clip";
        return;
    }
    const std::string sourcePath =
        pathString(resolvedClipMediaPathForProbe(*shellState, clip));
    const jcut::standalone_render::StandaloneMediaInfo mediaInfo =
        jcut::standalone_render::probeStandaloneMedia(sourcePath);
    if (!mediaInfo.probed || !mediaInfo.hasVideo) {
        shellState->statusMessage =
            "Auto Oppose could not open the selected clip";
        return;
    }
    const jcut::EditorAutoOpposeSettingsCore settings =
        shellState->autoOpposeSettings;
    const jcut::DecoderPolicySettingsCore decoderPolicy =
        shellState->decoderPolicy;
    const std::uint64_t documentGeneration =
        shellState->documentGeneration;
    const jcut::core::SizeI decodeSize = mediaInfo.frameSize.valid()
        ? mediaInfo.frameSize
        : jcut::core::SizeI{640, 360};
    shellState->autoOpposeRunning = true;
    shellState->autoOpposeClipId = clip.id;
    shellState->statusMessage = "Auto Oppose is analyzing clip frames";
    shellState->autoOpposeFuture = std::async(
        std::launch::async,
        [clip,
         sourcePath,
         settings,
         decoderPolicy,
         decodeSize,
         documentGeneration]() {
            AutoOpposeJobResult result;
            result.clipId = clip.id;
            result.documentGeneration = documentGeneration;
            const std::int64_t duration =
                std::max<std::int64_t>(1, clip.durationFrames);
            const int targetSamples =
                std::max(30, settings.sampleTarget);
            const std::int64_t sampleStep =
                std::max<std::int64_t>(1, duration / targetSamples);
            std::vector<jcut::EditorGradeProbeSampleCore> samples;
            samples.reserve(static_cast<std::size_t>(targetSamples + 4));
            jcut::standalone_render::StandaloneMediaFrameDecoder decoder(
                sourcePath, decoderPolicy);
            for (std::int64_t localFrame = 0;
                 localFrame < duration;
                 localFrame += sampleStep) {
                const double playbackRate =
                    std::isfinite(clip.playbackRate) &&
                        clip.playbackRate > 0.001
                    ? std::min(clip.playbackRate, 64.0)
                    : 1.0;
                std::int64_t sourceFrame =
                    std::max<std::int64_t>(0, clip.sourceInFrame) +
                    static_cast<std::int64_t>(std::llround(
                        localFrame * playbackRate));
                if (clip.sourceDurationFrames > 0) {
                    sourceFrame = std::min<std::int64_t>(
                        sourceFrame,
                        std::max<std::int64_t>(
                            0, clip.sourceDurationFrames - 1));
                }
                const auto decoded = decoder.decodeFrame(
                        static_cast<int>(std::min<std::int64_t>(
                            sourceFrame,
                            std::numeric_limits<int>::max())),
                        decodeSize);
                if (!decoded.success || decoded.image.empty()) {
                    continue;
                }
                jcut::EditorGradeProbeSampleCore sample;
                sample.localFrame = localFrame;
                if (jcut::probeEditorGradeStatsRgba(
                        decoded.image.bytes.data(),
                        decoded.image.size.width,
                        decoded.image.size.height,
                        decoded.image.strideBytes,
                        &sample)) {
                    samples.push_back(sample);
                }
            }
            result.decodedSamples = static_cast<int>(samples.size());
            if (samples.size() < 2) {
                result.message =
                    "Auto Oppose found fewer than two decodable samples";
                return result;
            }
            result.events =
                jcut::detectEditorOpposeGradeEvents(samples, settings);
            result.message = result.events.empty()
                ? "Auto Oppose found no major grade changes"
                : "Auto Oppose analysis completed";
            return result;
        });
}

void pollAutoOpposeJob(ShellState* shellState)
{
    if (!shellState->autoOpposeRunning ||
        !shellState->autoOpposeFuture.valid() ||
        shellState->autoOpposeFuture.wait_for(
            std::chrono::milliseconds(0)) != std::future_status::ready) {
        return;
    }
    AutoOpposeJobResult result;
    try {
        result = shellState->autoOpposeFuture.get();
    } catch (const std::exception& exception) {
        shellState->autoOpposeRunning = false;
        shellState->statusMessage =
            std::string("Auto Oppose failed: ") + exception.what();
        return;
    }
    shellState->autoOpposeRunning = false;
    if (result.documentGeneration != shellState->documentGeneration) {
        shellState->statusMessage =
            "Auto Oppose result discarded because the project changed";
        return;
    }
    if (result.events.empty()) {
        shellState->statusMessage = result.message;
        return;
    }
    jcut::EditorDocumentCore snapshot = runtimeSnapshot(shellState);
    const auto selected = std::find_if(
        snapshot.clips.begin(),
        snapshot.clips.end(),
        [&result](const jcut::EditorClip& candidate) {
            return candidate.id == result.clipId;
        });
    if (selected == snapshot.clips.end()) {
        shellState->statusMessage =
            "Auto Oppose result discarded because the clip was removed";
        return;
    }
    jcut::EditorClip workingClip = *selected;
    beginRuntimeHistoryTransaction(shellState);
    int appliedEvents = 0;
    for (const jcut::EditorOpposeGradeEventCore& event : result.events) {
        jcut::EditorGradingKeyframe keyframe =
            jcut::evaluateEditorClipGradingAtLocalFrame(
                workingClip, event.localFrame);
        keyframe.frame = std::clamp<std::int64_t>(
            event.localFrame,
            0,
            std::max<std::int64_t>(0, workingClip.durationFrames - 1));
        keyframe.brightness = std::clamp(
            keyframe.brightness + event.brightnessDelta, -10.0, 10.0);
        keyframe.contrast = std::clamp(
            keyframe.contrast * event.contrastMul, 0.05, 10.0);
        keyframe.saturation = std::clamp(
            keyframe.saturation * event.saturationMul, 0.0, 10.0);
        const jcut::CommandResult commandResult = applyCommand(
            shellState,
            jcut::UpsertGradingKeyframeCommand{
                workingClip.id, keyframe});
        if (!commandResult.applied) {
            break;
        }
        auto existing = std::find_if(
            workingClip.gradingKeyframes.begin(),
            workingClip.gradingKeyframes.end(),
            [&keyframe](const jcut::EditorGradingKeyframe& candidate) {
                return candidate.frame == keyframe.frame;
            });
        if (existing == workingClip.gradingKeyframes.end()) {
            workingClip.gradingKeyframes.push_back(keyframe);
        } else {
            *existing = keyframe;
        }
        ++appliedEvents;
    }
    endRuntimeHistoryTransaction(shellState);
    shellState->statusMessage =
        "Auto Oppose generated " + std::to_string(appliedEvents) +
        " opposing keyframe" + (appliedEvents == 1 ? "" : "s") +
        " from " + std::to_string(result.decodedSamples) + " samples";
}

void refreshClipMetadata(ShellState* shellState,
                         const jcut::EditorDocumentCore& snapshot,
                         int contextClipId)
{
    std::vector<jcut::EditorClipMetadataUpdate> updates;
    int missingCount = 0;
    const bool hasSelection = std::any_of(
        snapshot.clips.begin(),
        snapshot.clips.end(),
        [](const jcut::EditorClip& clip) { return clip.selected; });
    for (const jcut::EditorClip& clip : snapshot.clips) {
        if ((hasSelection ? !clip.selected : clip.id != contextClipId) ||
            jcut::canonicalEditorClipRole(clip.clipRole) == "mask_matte") {
            continue;
        }
        jcut::EditorClip sourceClip = clip;
        sourceClip.useProxy = false;
        sourceClip.proxyPath.clear();
        const fs::path sourcePath =
            resolvedClipMediaPathForProbe(*shellState, sourceClip);
        const auto mediaInfo =
            jcut::standalone_render::probeStandaloneMedia(
                pathString(sourcePath));
        if (!mediaInfo.probed) {
            ++missingCount;
            continue;
        }
        const double previousFps =
            clip.sourceFps > 0.001 ? clip.sourceFps : 30.0;
        const std::int64_t previousFullDuration =
            clip.sourceDurationFrames > 0
            ? std::max<std::int64_t>(
                  1,
                  std::llround(
                      static_cast<double>(clip.sourceDurationFrames) /
                      previousFps * 30.0))
            : 0;
        const bool lookedLikeFullSource =
            clip.sourceInFrame == 0 &&
            previousFullDuration > 0 &&
            std::abs(
                static_cast<std::int64_t>(clip.durationFrames) -
                previousFullDuration) <= 1;
        const double sourceFps =
            mediaInfo.videoFps > 0.001 ? mediaInfo.videoFps : previousFps;
        const std::int64_t sourceDuration =
            mediaInfo.sourceDurationFrames > 0
            ? mediaInfo.sourceDurationFrames
            : clip.sourceDurationFrames;
        int duration = clip.durationFrames;
        if (lookedLikeFullSource && mediaInfo.durationFrames > 0) {
            duration = static_cast<int>(std::clamp<std::int64_t>(
                mediaInfo.durationFrames,
                1,
                std::numeric_limits<int>::max()));
        }
        if (sourceDuration > 0) {
            const std::int64_t sourceIn = std::clamp<std::int64_t>(
                clip.sourceInFrame, 0, sourceDuration - 1);
            const std::int64_t availableTimelineFrames =
                std::max<std::int64_t>(
                    1,
                    std::llround(
                        static_cast<double>(sourceDuration - sourceIn) /
                        sourceFps * 30.0));
            duration = static_cast<int>(std::clamp<std::int64_t>(
                std::min<std::int64_t>(duration, availableTimelineFrames),
                1,
                std::numeric_limits<int>::max()));
        }
        updates.push_back({
            clip.id,
            mediaInfo.mediaKind,
            mediaInfo.hasAudio,
            sourceFps,
            sourceDuration,
            duration});
    }
    if (updates.empty()) {
        shellState->statusMessage =
            missingCount > 0
            ? "metadata refresh found no readable selected sources"
            : "metadata refresh found no selected clips";
        return;
    }
    const jcut::CommandResult result = applyCommand(
        shellState,
        jcut::RefreshClipMetadataCommand{std::move(updates)});
    if (result.applied && missingCount > 0) {
        shellState->statusMessage +=
            "; " + std::to_string(missingCount) + " source" +
            (missingCount == 1 ? "" : "s") + " missing";
    }
}

void startTranscriptionJob(ShellState* shellState,
                           const jcut::EditorClip& clip)
{
    jcut::EditorClip sourceClip = clip;
    sourceClip.useProxy = false;
    sourceClip.proxyPath.clear();
    const std::string mediaPath = pathString(
        resolvedClipMediaPathForProbe(*shellState, sourceClip));
    const std::string scriptPath = pathString(
        fs::absolute(fs::path("whisperx.sh")));
    std::string error;
    if (!shellState->transcriptionJob.start(
            {clip.id, scriptPath, mediaPath}, &error)) {
        shellState->statusMessage = error.empty()
            ? "transcription could not be started"
            : error;
        return;
    }
    shellState->requestedInspectorTab = "Jobs";
    shellState->statusMessage =
        "WhisperX transcription started";
}

void pollTranscriptionJob(ShellState* shellState)
{
    const jcut::jobs::TranscriptionJobSnapshotCore snapshot =
        shellState->transcriptionJob.snapshot();
    const int state = static_cast<int>(snapshot.state);
    if (state == shellState->transcriptionLastState) return;
    shellState->transcriptionLastState = state;
    if (!snapshot.status.empty()) {
        shellState->statusMessage = snapshot.status;
    }
    if (snapshot.state ==
            jcut::jobs::ProcessJobSnapshotCore::State::Completed &&
        snapshot.outputReady) {
        shellState->transcriptCache = {};
        shellState->requestedInspectorTab = "Transcript";
    }
}

void startBiRefNetJob(ShellState* shellState,
                      const jcut::EditorClip& clip)
{
    jcut::EditorClip sourceClip = clip;
    sourceClip.useProxy = false;
    sourceClip.proxyPath.clear();
    jcut::jobs::BiRefNetJobRequestCore request;
    request.clipId = clip.id;
    request.scriptPath =
        pathString(fs::absolute(fs::path("birefnet.sh")));
    request.mediaPath = pathString(
        resolvedClipMediaPathForProbe(*shellState, sourceClip));
    request.model = shellState->birefnetModel;
    request.revision = shellState->birefnetRevision;
    request.modelCachePath = shellState->birefnetModelCachePath;
    request.runtimeCachePath =
        shellState->birefnetRuntimeCachePath;
    request.device =
        shellState->birefnetDevice == 1 ? "cpu" : "cuda";
    request.fp16 = shellState->birefnetFp16;
    request.runDockerAsRoot =
        shellState->birefnetDockerRoot;
    request.restart = shellState->birefnetRestart;
    request.alphaTolerance = std::clamp(
        static_cast<double>(
            shellState->birefnetAlphaTolerancePercent) /
            100.0,
        0.0,
        0.99);
    std::string error;
    if (!shellState->birefnetJob.start(request, &error)) {
        shellState->statusMessage = error.empty()
            ? "BiRefNet could not be started"
            : error;
        return;
    }
    shellState->birefnetLivePreviewTextureId = 0;
    shellState->birefnetLivePreviewSize = {};
    shellState->birefnetLivePreviewLoadedPath.clear();
    shellState->birefnetLivePreviewHasStamp = false;
    shellState->birefnetLivePreviewError.clear();
    shellState->birefnetSourceClipId = clip.id;
    shellState->birefnetLastState = -1;
    shellState->requestedInspectorTab = "Jobs";
    shellState->statusMessage =
        "BiRefNet alpha generation started";
}

void pollBiRefNetJob(ShellState* shellState)
{
    const jcut::jobs::BiRefNetJobSnapshotCore snapshot =
        shellState->birefnetJob.snapshot();
    const int state = static_cast<int>(snapshot.state);
    if (state == shellState->birefnetLastState) return;
    shellState->birefnetLastState = state;
    if (!snapshot.status.empty()) {
        shellState->statusMessage = snapshot.status;
    }
    if (snapshot.state ==
            jcut::jobs::ProcessJobSnapshotCore::State::Completed &&
        snapshot.outputReady &&
        shellState->birefnetSourceClipId > 0) {
        applyCommand(
            shellState,
            jcut::MaterializeMaskMatteCommand{
                shellState->birefnetSourceClipId,
                snapshot.outputDirectory,
                "birefnet-alpha",
                "BiRefNet Alpha"});
        shellState->maskSidecarContextClipId = -1;
        shellState->requestedInspectorTab = "Masks";
    }
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

bool selectedClipsCanSplitAtFrame(
    const jcut::EditorDocumentCore& snapshot,
    std::int64_t frame)
{
    return std::any_of(
        snapshot.clips.begin(),
        snapshot.clips.end(),
        [frame](const jcut::EditorClip& clip) {
            return clip.selected && !clip.locked &&
                jcut::canonicalEditorClipRole(clip.clipRole) !=
                    "mask_matte" &&
                frame > clip.startFrame &&
                frame < static_cast<std::int64_t>(clip.startFrame) +
                    clip.durationFrames;
        });
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

struct PipelineStageCore {
    std::string label;
    std::string kind;
    std::string state;
    std::string detail;
    bool active = false;
    bool exact = false;
    std::vector<std::pair<std::string, std::string>> facts;
};

std::vector<PipelineStageCore> previewPipelineStages(
    const jcut::standalone_render::PreviewRenderResult& result,
    bool lastUsedZeroCopy,
    bool zeroCopyAvailable,
    const std::string& zeroCopyFailure)
{
    std::vector<PipelineStageCore> stages;
    stages.push_back({
        "Timeline Map", "mapping",
        result.sourcePath.empty() ? "idle" : "ready",
        result.sourcePath.empty()
            ? "No active source at the playhead"
            : "Timeline/source timing resolved",
        !result.sourcePath.empty(),
        true,
        {
            {"Source", result.sourcePath.empty() ? "-" : result.sourcePath},
        }});
    stages.push_back({
        "Decode", "decoder",
        result.success ? "ready" : "blocked",
        result.message.empty() ? "Waiting for decode" : result.message,
        result.success,
        result.success,
        {
            {"Preference", jcut::decodePreferenceCoreName(
                result.effectiveDecodePreference)},
            {"Hardware", result.hardwareAccelerated ? "yes" : "no"},
            {"Device", result.hardwareDeviceLabel.empty()
                 ? "software" : result.hardwareDeviceLabel},
            {"Retained hardware frame",
             result.hardwareFrame ? "yes" : "no"},
        }});
    stages.push_back({
        "GPU Import", "surface",
        lastUsedZeroCopy
            ? "live exact"
            : (zeroCopyAvailable ? "fallback" : "blocked"),
        lastUsedZeroCopy
            ? "External Vulkan frame is bound directly"
            : (zeroCopyFailure.empty()
                   ? "No importable external frame"
                   : zeroCopyFailure),
        lastUsedZeroCopy,
        lastUsedZeroCopy,
        {
            {"GPU frame", result.vulkanFrame.valid ? "valid" : "none"},
            {"Direct eligible",
             result.hardwareDirectEligible ? "yes" : "no"},
            {"Available", zeroCopyAvailable ? "yes" : "no"},
        }});
    stages.push_back({
        "Composite", "composite",
        !result.image.empty()
            ? "live exact"
            : (lastUsedZeroCopy ? "bypassed" : "blocked"),
        !result.image.empty()
            ? "Standalone rich composition produced a CPU frame"
            : (lastUsedZeroCopy
                   ? "Direct frame bypassed CPU composition"
                   : "No composited frame"),
        !result.image.empty(),
        !result.image.empty(),
        {
            {"CPU frame", result.image.empty() ? "none" : "valid"},
            {"Fallback reason",
             result.hardwareDirectFallbackReason.empty()
                 ? "-" : result.hardwareDirectFallbackReason},
        }});
    stages.push_back({
        "Present", "surface",
        result.success ? "ready" : "blocked",
        lastUsedZeroCopy
            ? "Vulkan external image → X11 swapchain"
            : "Uploaded RGBA image → X11 Vulkan swapchain",
        result.success,
        true,
        {
            {"Path", lastUsedZeroCopy ? "zero-copy" : "CPU upload"},
            {"Window", "raw X11/Vulkan"},
        }});
    return stages;
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
    jcut::H26xThreadingModeCore threading =
        jcut::H26xThreadingModeCore::Auto;
    (void)jcut::parseH26xThreadingModeCore(
        legacyStringValue(
            *shellState, "debugH26xSoftwareThreadingMode"),
        &threading);
    shellState->decoderPolicy.h26xThreadingMode = threading;
    shellState->decoderPolicy.deterministic = legacyBoolValue(
        *shellState, "debugDeterministicPipeline", false);
    shellState->decoderPolicy.decoderLaneCount = legacyIntValue(
        *shellState, "debugDecoderLaneCount", 0, 0, 16);
    shellState->featureAiPanel = legacyBoolValue(
        *shellState, "feature_ai_panel", true);
    shellState->featureAiSpeakerCleanup = legacyBoolValue(
        *shellState, "feature_ai_speaker_cleanup", true);
    const std::string configuredGateway =
        legacyStringValue(*shellState, "aiProxyBaseUrl");
    if (!configuredGateway.empty()) {
        shellState->aiGatewayBaseUrl =
            jcut::ai::normalizeGatewayBaseUrl(configuredGateway);
    } else if (const char* environmentGateway = std::getenv("SUPABASE_URL");
               environmentGateway && *environmentGateway) {
        shellState->aiGatewayBaseUrl =
            jcut::ai::normalizeGatewayBaseUrl(environmentGateway);
    }
    const std::string configuredModel =
        legacyStringValue(*shellState, "aiSelectedModel");
    if (!configuredModel.empty()) {
        shellState->aiSelectedModel = configuredModel;
    }
    shellState->aiUsageBudgetCap = legacyIntValue(
        *shellState, "aiUsageBudgetCap", 200, 1, 1000000);
    shellState->aiUsageRequests = legacyIntValue(
        *shellState, "aiUsageRequests", 0, 0, 1000000);
    shellState->aiUsageFailures = legacyIntValue(
        *shellState, "aiUsageFailures", 0, 0, 1000000);
    if (shellState->aiSessionToken.empty()) {
        if (const char* environmentToken = std::getenv("JCUT_AI_AUTH_TOKEN");
            environmentToken && *environmentToken) {
            shellState->aiSessionToken = environmentToken;
            shellState->aiCredentialStatus =
                "Using JCUT_AI_AUTH_TOKEN for this session.";
            shellState->aiCredentialLoadAttempted = true;
        } else if (!shellState->aiCredentialLoadAttempted) {
            shellState->aiCredentialLoadAttempted = true;
            const jcut::ai::CredentialStoreResultCore credentials =
                jcut::ai::loadStoredCredentialsCore();
            if (credentials.ok &&
                !credentials.credentials.accessToken.empty()) {
                shellState->aiSessionToken =
                    credentials.credentials.accessToken;
                shellState->aiRefreshToken =
                    credentials.credentials.refreshToken;
                shellState->aiUserId =
                    credentials.credentials.userId;
                shellState->aiCredentialStatus =
                    credentials.usedSystemStore
                    ? "Loaded credentials from the system secret store."
                    : "Loaded credentials from the private config fallback.";
            } else if (!credentials.error.empty()) {
                shellState->aiCredentialStatus = credentials.error;
            }
        }
    }
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
        {"debugH26xSoftwareThreadingMode",
         jcut::h26xThreadingModeCoreName(
             shellState.decoderPolicy.h26xThreadingMode)},
        {"debugDeterministicPipeline",
         shellState.decoderPolicy.deterministic},
        {"debugDecoderLaneCount",
         shellState.decoderPolicy.decoderLaneCount},
        {"aiProxyBaseUrl", shellState.aiGatewayBaseUrl},
        {"aiSelectedModel", shellState.aiSelectedModel},
        {"feature_ai_panel", shellState.featureAiPanel},
        {"feature_ai_speaker_cleanup",
         shellState.featureAiSpeakerCleanup},
        {"aiUsageBudgetCap", shellState.aiUsageBudgetCap},
        {"aiUsageRequests", shellState.aiUsageRequests},
        {"aiUsageFailures", shellState.aiUsageFailures},
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
            {"requestedBufferFrames", audioStatus.requestedBufferFrames},
            {"actualBufferFrames", audioStatus.actualBufferFrames},
            {"requestedOutputDevice",
             audioStatus.requestedOutputDeviceName},
            {"activeOutputDevice", audioStatus.activeOutputDeviceName},
            {"scheduledSourcePaths", audioStatus.scheduledSourcePaths},
            {"status", audioStatus.message}
        }},
        {"preview", {
            {"generation", previewCompletedGeneration},
            {"success", previewResult.success},
            {"message", previewResult.message},
            {"sourcePath", previewResult.sourcePath},
            {"decode", {
                {"requested", jcut::decodePreferenceCoreName(
                    previewResult.requestedDecodePreference)},
                {"requestedDevice",
                 jcut::decodeHardwareDeviceCoreName(
                     shellState->decoderPolicy.hardwareDevice)},
                {"effective", jcut::decodePreferenceCoreName(
                    previewResult.effectiveDecodePreference)},
                {"hardwareAccelerated",
                 previewResult.hardwareAccelerated},
                {"device", previewResult.hardwareDeviceLabel},
                {"fallbackReason",
                 previewResult.hardwareFallbackReason}
            }},
            {"zeroCopyVulkan", {
                {"ready", previewZeroCopyAvailable},
                {"presentedFrames", previewLastUsedZeroCopy ? previewCompletedGeneration : 0},
                {"failures", previewZeroCopyFailureReason.empty() ? 0 : 1},
                {"failureReason", previewZeroCopyFailureReason},
                {"hardwareFrameRetained",
                 static_cast<bool>(previewResult.hardwareFrame)},
                {"hardwareDirectEligible",
                 previewResult.hardwareDirectEligible},
                {"hardwareDirectFallbackReason",
                 previewResult.hardwareDirectFallbackReason},
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
    shellState->timelineContextClickFrame = 0;
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
        jcut::DecoderPolicySettingsCore decoderPolicy;
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
            decoderPolicy = shellState->previewDecoderPolicy;
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
                    true,
                    decoderPolicy});

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
        std::vector<ShellState::QueuedExport> queue;
        std::string rootDirectory;
        std::uint64_t generation = 0;
        jcut::DecoderPolicySettingsCore decoderPolicy;
        {
            std::unique_lock<std::mutex> lock(shellState->exportMutex);
            shellState->exportCondition.wait(lock, [shellState]() {
                return shellState->exportStopRequested || shellState->exportRequested;
            });
            if (shellState->exportStopRequested && !shellState->exportRequested) {
                return;
            }
            queue = std::move(shellState->exportQueue);
            shellState->exportQueue.clear();
            if (queue.empty()) {
                queue.push_back({
                    shellState->exportDocument,
                    "Export",
                });
            }
            shellState->exportQueueTotal = queue.size();
            rootDirectory = shellState->exportRootDirectory;
            generation = shellState->exportRequestGeneration;
            decoderPolicy = shellState->exportDecoderPolicy;
            shellState->exportRequested = false;
            shellState->exportRunning = true;
            shellState->exportHasProgress = false;
            shellState->exportProgress = {};
            shellState->exportResult = {};
        }

        jcut::render::RenderResultCore summary;
        summary.success = true;
        std::size_t completed = 0;
        std::size_t failed = 0;
        for (std::size_t index = 0;
             index < queue.size();
             ++index) {
            {
                std::lock_guard<std::mutex> lock(
                    shellState->exportMutex);
                if (shellState->exportCancelRequested ||
                    shellState->exportStopRequested) {
                    summary.cancelled = true;
                    summary.success = false;
                    break;
                }
                shellState->exportQueueCurrent = index + 1;
                shellState->exportQueueLabel =
                    queue[index].label;
            }
            const jcut::render::RenderResultCore result =
                jcut::standalone_render::exportTimelineToFile(
                    jcut::standalone_render::ExportRenderRequest{
                        queue[index].document,
                        rootDirectory,
                        0,
                        0,
                        decoderPolicy},
                    [shellState](
                        const jcut::render::RenderProgressCore&
                            progress) {
                        std::lock_guard<std::mutex> lock(
                            shellState->exportMutex);
                        shellState->exportProgress = progress;
                        shellState->exportHasProgress = true;
                        return !shellState->
                                    exportCancelRequested &&
                            !shellState->exportStopRequested;
                    });
            summary.framesRendered +=
                result.framesRendered;
            summary.elapsedMs += result.elapsedMs;
            summary.encoderLabel = result.encoderLabel;
            summary.usedGpu =
                summary.usedGpu || result.usedGpu;
            summary.usedHardwareEncode =
                summary.usedHardwareEncode ||
                result.usedHardwareEncode;
            if (result.success) {
                ++completed;
            } else {
                ++failed;
                summary.success = false;
                if (result.cancelled) {
                    summary.cancelled = true;
                }
                if (!result.message.empty()) {
                    summary.message = result.message;
                }
            }
            {
                std::lock_guard<std::mutex> lock(
                    shellState->exportMutex);
                shellState->exportResult = result;
                shellState->exportQueueCompleted = completed;
                shellState->exportQueueFailed = failed;
            }
            if (result.cancelled) break;
        }
        if (summary.cancelled) {
            summary.message = "export batch cancelled after " +
                std::to_string(completed) + " completed";
        } else if (failed > 0) {
            summary.message = "export batch finished: " +
                std::to_string(completed) + " completed, " +
                std::to_string(failed) + " failed";
        } else if (queue.size() > 1) {
            summary.message = "export batch completed: " +
                std::to_string(completed) + " files";
        } else if (summary.message.empty()) {
            summary.message = "export completed";
        }

        {
            std::lock_guard<std::mutex> lock(shellState->exportMutex);
            shellState->exportResult = summary;
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
    std::vector<jcut::timeline::SnapClip> clips;
    clips.reserve(snapshot.clips.size());
    for (const jcut::EditorClip& clip : snapshot.clips) {
        if (jcut::canonicalEditorClipRole(clip.clipRole) ==
            "mask_matte") {
            continue;
        }
        clips.push_back({
            clip.id,
            clip.startFrame,
            clip.durationFrames,
            clip.selected});
    }
    const jcut::timeline::GroupMoveSnap result =
        jcut::timeline::snapSelectedGroupMove(
            clips,
            anchorClipId,
            proposedStartFrame,
            timelineSnapThresholdFrames());
    return {
        static_cast<int>(std::clamp<std::int64_t>(
            result.anchorStartFrame,
            0,
            std::numeric_limits<int>::max())),
        result.boundaryFrame >= 0
            ? static_cast<int>(std::clamp<std::int64_t>(
                  result.boundaryFrame,
                  0,
                  std::numeric_limits<int>::max()))
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
    struct AuxiliaryTexture {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        jcut::core::SizeI size{};
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkDescriptorSet descriptor = VK_NULL_HANDLE;
    };

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
    jcut::vulkan_import::VulkanHardwareFrameImportCore
        hardwareFrameHandoff;
    bool hardwareFrameHandoffInitialized = false;
    std::string hardwareFrameHandoffStatus;
    VkDescriptorSet previewTextureSet = VK_NULL_HANDLE;
    VkImageView boundPreviewView = VK_NULL_HANDLE;
    VkImageLayout boundPreviewLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    AuxiliaryTexture birefnetLivePreviewTexture;
    AuxiliaryTexture mediaThumbnailTexture;
    AuxiliaryTexture aiProfileAvatarTexture;
    AuxiliaryTexture faceReferenceTexture;
    AuxiliaryTexture sectionAvatarTexture;
    AuxiliaryTexture previewOverlayTexture;

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

    void transitionImage(VkCommandBuffer commandBuffer,
                         VkImage image,
                         VkImageLayout oldLayout,
                         VkImageLayout newLayout)
    {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
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

    void transitionPreviewImage(VkCommandBuffer commandBuffer,
                                VkImageLayout oldLayout,
                                VkImageLayout newLayout)
    {
        transitionImage(
            commandBuffer, previewImage, oldLayout, newLayout);
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

    void releaseAuxiliaryTexture(AuxiliaryTexture* texture)
    {
        if (!texture || device == VK_NULL_HANDLE) return;
        if (texture->descriptor != VK_NULL_HANDLE) {
            ImGui_ImplVulkan_RemoveTexture(texture->descriptor);
            texture->descriptor = VK_NULL_HANDLE;
        }
        if (texture->view != VK_NULL_HANDLE) {
            vkDestroyImageView(device, texture->view, nullptr);
            texture->view = VK_NULL_HANDLE;
        }
        if (texture->image != VK_NULL_HANDLE) {
            vkDestroyImage(device, texture->image, nullptr);
            texture->image = VK_NULL_HANDLE;
        }
        if (texture->memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, texture->memory, nullptr);
            texture->memory = VK_NULL_HANDLE;
        }
        texture->size = {};
        texture->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    bool createAuxiliaryTexture(
        AuxiliaryTexture* texture,
        const jcut::core::SizeI& size,
        std::string* error)
    {
        if (!texture || !size.valid()) {
            if (error) *error = "Invalid auxiliary texture size.";
            return false;
        }
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent = {
            static_cast<uint32_t>(size.width),
            static_cast<uint32_t>(size.height),
            1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage =
            VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateImage(
                device, &imageInfo, nullptr,
                &texture->image) != VK_SUCCESS) {
            if (error) *error =
                "Failed to create Vulkan auxiliary image.";
            return false;
        }
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(
            device, texture->image, &requirements);
        const uint32_t memoryType = findMemoryType(
            requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (memoryType == UINT32_MAX) {
            if (error) *error =
                "No suitable memory type for Vulkan auxiliary image.";
            releaseAuxiliaryTexture(texture);
            return false;
        }
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = requirements.size;
        allocInfo.memoryTypeIndex = memoryType;
        if (vkAllocateMemory(
                device, &allocInfo, nullptr,
                &texture->memory) != VK_SUCCESS) {
            if (error) *error =
                "Failed to allocate Vulkan auxiliary image memory.";
            releaseAuxiliaryTexture(texture);
            return false;
        }
        if (vkBindImageMemory(
                device, texture->image, texture->memory, 0) !=
            VK_SUCCESS) {
            if (error) *error =
                "Failed to bind Vulkan auxiliary image memory.";
            releaseAuxiliaryTexture(texture);
            return false;
        }
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType =
            VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = texture->image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange.aspectMask =
            VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        if (vkCreateImageView(
                device, &viewInfo, nullptr,
                &texture->view) != VK_SUCCESS) {
            if (error) *error =
                "Failed to create Vulkan auxiliary image view.";
            releaseAuxiliaryTexture(texture);
            return false;
        }
        texture->size = size;
        texture->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        return true;
    }

    bool uploadAuxiliaryImage(
        const jcut::core::ImageBuffer& image,
        AuxiliaryTexture* texture,
        ImTextureID* textureOut,
        std::string* error)
    {
        if (!texture || image.empty() ||
            image.format != jcut::core::PixelFormat::Rgba8) {
            if (error) *error = "Invalid CPU auxiliary image.";
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
                std::memcpy(
                    packed.data() +
                        static_cast<std::size_t>(
                            y * image.size.width * 4),
                    image.bytes.data() +
                        static_cast<std::size_t>(
                            y * image.strideBytes),
                    static_cast<std::size_t>(
                        image.size.width * 4));
            }
            uploadBytes = packed.data();
        }
        VkBuffer stagingBuffer = VK_NULL_HANDLE;
        VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
        if (!createBuffer(
                byteCount,
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                &stagingBuffer,
                &stagingMemory,
                error)) {
            return false;
        }
        const auto releaseStaging = [&]() {
            vkDestroyBuffer(device, stagingBuffer, nullptr);
            vkFreeMemory(device, stagingMemory, nullptr);
        };
        void* mapped = nullptr;
        if (vkMapMemory(
                device, stagingMemory, 0, byteCount, 0,
                &mapped) != VK_SUCCESS ||
            !mapped) {
            if (error) *error =
                "Failed to map Vulkan auxiliary upload memory.";
            releaseStaging();
            return false;
        }
        std::memcpy(
            mapped, uploadBytes,
            static_cast<std::size_t>(byteCount));
        vkUnmapMemory(device, stagingMemory);
        if (texture->image == VK_NULL_HANDLE ||
            texture->size.width != image.size.width ||
            texture->size.height != image.size.height) {
            vkDeviceWaitIdle(device);
            releaseAuxiliaryTexture(texture);
            if (!createAuxiliaryTexture(
                    texture, image.size, error)) {
                releaseStaging();
                return false;
            }
        }
        VkCommandBuffer commands = beginUploadCommands(error);
        if (commands == VK_NULL_HANDLE) {
            releaseStaging();
            return false;
        }
        transitionImage(
            commands,
            texture->image,
            texture->layout,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy copyRegion{};
        copyRegion.imageSubresource.aspectMask =
            VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.imageSubresource.layerCount = 1;
        copyRegion.imageExtent = {
            static_cast<uint32_t>(image.size.width),
            static_cast<uint32_t>(image.size.height),
            1};
        vkCmdCopyBufferToImage(
            commands,
            stagingBuffer,
            texture->image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1,
            &copyRegion);
        transitionImage(
            commands,
            texture->image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        const bool submitted =
            endUploadCommands(commands, error);
        releaseStaging();
        if (!submitted) return false;
        texture->layout =
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        if (texture->descriptor == VK_NULL_HANDLE) {
            texture->descriptor = ImGui_ImplVulkan_AddTexture(
                previewSampler,
                texture->view,
                texture->layout);
        }
        if (textureOut) {
            *textureOut = reinterpret_cast<ImTextureID>(
                texture->descriptor);
        }
        return texture->descriptor != VK_NULL_HANDLE;
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
        handoffError.clear();
        hardwareFrameHandoffInitialized =
            hardwareFrameHandoff.initialize(
                physicalDevice,
                device,
                queue,
                queueFamily,
                JCUT_VULKAN_SHADER_DIR,
                &handoffError);
        hardwareFrameHandoffStatus =
            hardwareFrameHandoffInitialized
            ? std::string("ready")
            : (handoffError.empty()
                ? std::string("decoded hardware-frame import unavailable")
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

    bool bindHardwarePreviewFrame(
        const jcut::core::FramePayloadCore& frame,
        const jcut::EditorGradingKeyframe& grade,
        ImTextureID* textureOut,
        std::string* error)
    {
        if (!hardwareFrameHandoffInitialized) {
            if (error) {
                *error = hardwareFrameHandoffStatus.empty()
                    ? "decoded hardware-frame import unavailable"
                    : hardwareFrameHandoffStatus;
            }
            return false;
        }
        jcut::vulkan_import::HardwareFrameColorGradeCore hardwareGrade;
        hardwareGrade.brightness = static_cast<float>(grade.brightness);
        hardwareGrade.contrast = static_cast<float>(grade.contrast);
        hardwareGrade.saturation = static_cast<float>(grade.saturation);
        hardwareGrade.shadowsR = static_cast<float>(grade.shadowsR);
        hardwareGrade.shadowsG = static_cast<float>(grade.shadowsG);
        hardwareGrade.shadowsB = static_cast<float>(grade.shadowsB);
        hardwareGrade.midtonesR = static_cast<float>(grade.midtonesR);
        hardwareGrade.midtonesG = static_cast<float>(grade.midtonesG);
        hardwareGrade.midtonesB = static_cast<float>(grade.midtonesB);
        hardwareGrade.highlightsR =
            static_cast<float>(grade.highlightsR);
        hardwareGrade.highlightsG =
            static_cast<float>(grade.highlightsG);
        hardwareGrade.highlightsB =
            static_cast<float>(grade.highlightsB);
        hardwareGrade.curvesEnabled =
            !jcut::editorGradingCurveIsIdentity(grade.curvePointsR) ||
            !jcut::editorGradingCurveIsIdentity(grade.curvePointsG) ||
            !jcut::editorGradingCurveIsIdentity(grade.curvePointsB) ||
            !jcut::editorGradingCurveIsIdentity(grade.curvePointsLuma);
        if (hardwareGrade.curvesEnabled) {
            hardwareGrade.curveLut =
                jcut::editorPackedGradingCurveLut(grade);
        }
        if (!hardwareFrameHandoff.importFrame(
                frame, hardwareGrade, error)) {
            return false;
        }
        const jcut::vulkan_import::ExternalImage external =
            hardwareFrameHandoff.externalImage();
        if (external.imageView == VK_NULL_HANDLE ||
            !external.size.valid()) {
            if (error) {
                *error =
                    "decoded hardware-frame Vulkan image is unavailable";
            }
            return false;
        }
        if (previewTextureSet == VK_NULL_HANDLE ||
            boundPreviewView != external.imageView ||
            boundPreviewLayout != external.imageLayout) {
            if (previewTextureSet != VK_NULL_HANDLE) {
                ImGui_ImplVulkan_RemoveTexture(previewTextureSet);
                previewTextureSet = VK_NULL_HANDLE;
            }
            previewTextureSet = ImGui_ImplVulkan_AddTexture(
                previewSampler,
                external.imageView,
                external.imageLayout);
            boundPreviewView = external.imageView;
            boundPreviewLayout = external.imageLayout;
        }
        if (textureOut) {
            *textureOut =
                reinterpret_cast<ImTextureID>(previewTextureSet);
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
        releaseAuxiliaryTexture(
            &birefnetLivePreviewTexture);
        releaseAuxiliaryTexture(
            &mediaThumbnailTexture);
        releaseAuxiliaryTexture(
            &aiProfileAvatarTexture);
        releaseAuxiliaryTexture(
            &faceReferenceTexture);
        releaseAuxiliaryTexture(
            &sectionAvatarTexture);
        releaseAuxiliaryTexture(
            &previewOverlayTexture);
        releasePreviewTextureResources();
        hardwareFrameHandoff.release();
        hardwareFrameHandoffInitialized = false;
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
    } else if (vulkanShell && previewResult.hardwareFrame) {
        ImTextureID texture = 0;
        std::string error;
        if (vulkanShell->bindHardwarePreviewFrame(
                *previewResult.hardwareFrame,
                previewResult.hardwarePresentationGrade,
                &texture,
                &error)) {
            shellState->previewTextureId = texture;
            uploadedTexture = true;
            usedZeroCopy = true;
            zeroCopyAvailable = true;
        } else {
            zeroCopyAvailable = false;
            zeroCopyFailureReason = error.empty()
                ? "decoded hardware-frame Vulkan handoff failed"
                : error;
            if (previewResult.image.empty()) {
                requestCpuFallbackFrame = true;
            }
        }
    } else if (!previewResult.vulkanFrame.valid) {
        zeroCopyFailureReason = "preview renderer did not return a Vulkan frame";
        if (previewResult.image.empty()) {
            requestCpuFallbackFrame = true;
        }
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
    shellState->previewOverlayTextureId = 0;
    shellState->previewOverlaySize = {};
    shellState->previewOverlayX = 0;
    shellState->previewOverlayY = 0;
    if (usedZeroCopy &&
        vulkanShell &&
        !previewResult.hardwareOverlayImage.empty()) {
        ImTextureID overlayTexture = 0;
        std::string overlayError;
        if (vulkanShell->uploadAuxiliaryImage(
                previewResult.hardwareOverlayImage,
                &vulkanShell->previewOverlayTexture,
                &overlayTexture,
                &overlayError)) {
            shellState->previewOverlayTextureId =
                overlayTexture;
            shellState->previewOverlaySize =
                previewResult.hardwareOverlayImage.size;
            shellState->previewOverlayX =
                previewResult.hardwareOverlayX;
            shellState->previewOverlayY =
                previewResult.hardwareOverlayY;
        } else {
            requestCpuFallbackFrame = true;
            usedZeroCopy = false;
            zeroCopyAvailable = false;
            uploadedTexture = false;
            zeroCopyFailureReason =
                overlayError.empty()
                ? "Vulkan transcript overlay upload failed"
                : std::move(overlayError);
        }
    }

    if (!uploadedTexture) {
        shellState->previewTextureId = 0;
    }
    shellState->previewHardwarePresentationTransformValid =
        usedZeroCopy &&
        static_cast<bool>(previewResult.hardwareFrame) &&
        previewResult.hardwarePresentationTransformValid;
    shellState->previewHardwarePresentationTransform =
        previewResult.hardwarePresentationTransform;
    shellState->previewHardwarePresentationOpacity =
        std::clamp(
            previewResult.hardwarePresentationOpacity,
            0.0,
            1.0);
    shellState->previewHardwareSourceSize =
        shellState->previewHardwarePresentationTransformValid &&
            previewResult.hardwareFrame
        ? previewResult.hardwareFrame->size()
        : jcut::core::SizeI{};

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

void refreshBiRefNetLivePreviewTexture(
    ShellState* shellState,
    VulkanShell* vulkanShell)
{
    if (!shellState || !vulkanShell) return;
    const auto now = std::chrono::steady_clock::now();
    if (now <
        shellState->nextBiRefNetLivePreviewRefresh) {
        return;
    }
    shellState->nextBiRefNetLivePreviewRefresh =
        now + std::chrono::milliseconds(250);
    const jcut::jobs::BiRefNetJobSnapshotCore snapshot =
        shellState->birefnetJob.snapshot();
    if (snapshot.livePreviewPath.empty()) return;
    const fs::path previewPath(snapshot.livePreviewPath);
    std::error_code error;
    if (!fs::is_regular_file(previewPath, error) || error) {
        return;
    }
    error.clear();
    const std::uintmax_t fileSize =
        fs::file_size(previewPath, error);
    if (error || fileSize == 0) return;
    error.clear();
    const fs::file_time_type modified =
        fs::last_write_time(previewPath, error);
    if (error) return;
    if (shellState->birefnetLivePreviewHasStamp &&
        shellState->birefnetLivePreviewLoadedPath ==
            snapshot.livePreviewPath &&
        shellState->birefnetLivePreviewLoadedSize ==
            fileSize &&
        shellState->birefnetLivePreviewLoadedTime ==
            modified) {
        return;
    }
    jcut::core::SizeI decodeSize{640, 360};
    const jcut::standalone_render::StandaloneMediaInfo info =
        jcut::standalone_render::probeStandaloneMedia(
            snapshot.livePreviewPath);
    if (info.frameSize.valid()) {
        const double scale = std::min(
            1.0,
            std::min(
                1280.0 /
                    static_cast<double>(
                        info.frameSize.width),
                720.0 /
                    static_cast<double>(
                        info.frameSize.height)));
        decodeSize = {
            std::max(
                1,
                static_cast<int>(std::lround(
                    info.frameSize.width * scale))),
            std::max(
                1,
                static_cast<int>(std::lround(
                    info.frameSize.height * scale)))};
    }
    jcut::DecoderPolicySettingsCore policy =
        shellState->previewDecoderPolicy;
    policy.decodePreference =
        jcut::DecodePreferenceCore::Software;
    const auto decoded =
        jcut::standalone_render::decodeStandaloneMediaFrame(
            snapshot.livePreviewPath,
            0,
            decodeSize,
            policy);
    if (!decoded.success || decoded.image.empty()) {
        shellState->birefnetLivePreviewError =
            decoded.message.empty()
            ? "BiRefNet live preview is not yet readable"
            : decoded.message;
        return;
    }
    ImTextureID texture = 0;
    std::string uploadError;
    if (!vulkanShell->uploadAuxiliaryImage(
            decoded.image,
            &vulkanShell->birefnetLivePreviewTexture,
            &texture,
            &uploadError)) {
        shellState->birefnetLivePreviewError =
            uploadError.empty()
            ? "BiRefNet live preview upload failed"
            : std::move(uploadError);
        return;
    }
    shellState->birefnetLivePreviewTextureId = texture;
    shellState->birefnetLivePreviewSize =
        decoded.image.size;
    shellState->birefnetLivePreviewLoadedPath =
        snapshot.livePreviewPath;
    shellState->birefnetLivePreviewLoadedSize = fileSize;
    shellState->birefnetLivePreviewLoadedTime = modified;
    shellState->birefnetLivePreviewHasStamp = true;
    shellState->birefnetLivePreviewError.clear();
}

void refreshMediaThumbnailTexture(
    ShellState* shellState,
    VulkanShell* vulkanShell)
{
    if (!shellState || !vulkanShell) return;
    if (shellState->mediaThumbnailRunning &&
        shellState->mediaThumbnailFuture.valid() &&
        shellState->mediaThumbnailFuture.wait_for(
            std::chrono::seconds(0)) == std::future_status::ready) {
        const auto decoded =
            shellState->mediaThumbnailFuture.get();
        shellState->mediaThumbnailRunning = false;
        if (!decoded.success || decoded.image.empty()) {
            shellState->mediaThumbnailError =
                decoded.message.empty()
                    ? "Media thumbnail could not be decoded."
                    : decoded.message;
        } else {
            ImTextureID texture = 0;
            std::string uploadError;
            if (vulkanShell->uploadAuxiliaryImage(
                    decoded.image,
                    &vulkanShell->mediaThumbnailTexture,
                    &texture,
                    &uploadError)) {
                shellState->mediaThumbnailTextureId = texture;
                shellState->mediaThumbnailSize =
                    decoded.image.size;
                shellState->mediaThumbnailLoadedPath =
                    shellState->mediaThumbnailPendingPath;
                shellState->mediaThumbnailError.clear();
            } else {
                shellState->mediaThumbnailError =
                    uploadError.empty()
                        ? "Media thumbnail upload failed."
                        : std::move(uploadError);
            }
        }
    }

    const std::string requestedPath =
        !shellState->mediaHoveredPath.empty()
            ? shellState->mediaHoveredPath
            : shellState->mediaSelectedPath;
    if (shellState->mediaThumbnailRunning ||
        requestedPath.empty() ||
        requestedPath == shellState->mediaThumbnailLoadedPath ||
        requestedPath == shellState->mediaThumbnailPendingPath ||
        !isImportableMediaPath(requestedPath)) {
        return;
    }
    shellState->mediaThumbnailPendingPath = requestedPath;
    shellState->mediaThumbnailRunning = true;
    shellState->mediaThumbnailError.clear();
    jcut::DecoderPolicySettingsCore policy =
        shellState->previewDecoderPolicy;
    policy.decodePreference = jcut::DecodePreferenceCore::Software;
    shellState->mediaThumbnailFuture = std::async(
        std::launch::async,
        [requestedPath, policy]() {
            jcut::core::SizeI decodeSize{480, 270};
            const auto info =
                jcut::standalone_render::probeStandaloneMedia(
                    requestedPath);
            if (info.frameSize.valid()) {
                const double scale = std::min(
                    1.0,
                    std::min(
                        480.0 /
                            static_cast<double>(
                                info.frameSize.width),
                        270.0 /
                            static_cast<double>(
                                info.frameSize.height)));
                decodeSize = {
                    std::max(
                        1,
                        static_cast<int>(std::lround(
                            info.frameSize.width * scale))),
                    std::max(
                        1,
                        static_cast<int>(std::lround(
                            info.frameSize.height * scale)))};
            }
            return jcut::standalone_render::
                decodeStandaloneMediaFrame(
                    requestedPath, 0, decodeSize, policy);
        });
}

void refreshAiProfileAvatarTexture(
    ShellState* shellState,
    VulkanShell* vulkanShell)
{
    if (!shellState || !vulkanShell) return;
    const jcut::ai::AccessTokenProfileCore profile =
        jcut::ai::parseAccessTokenProfileCore(
            shellState->aiSessionToken);
    const std::string desiredUrl = profile.avatarUrl;

    if (!shellState->aiAvatarLoadedUrl.empty() &&
        shellState->aiAvatarLoadedUrl != desiredUrl) {
        vulkanShell->releaseAuxiliaryTexture(
            &vulkanShell->aiProfileAvatarTexture);
        shellState->aiAvatarTextureId = 0;
        shellState->aiAvatarSize = {};
        shellState->aiAvatarLoadedUrl.clear();
        if (!shellState->aiAvatarCachePath.empty()) {
            std::error_code ignored;
            fs::remove(shellState->aiAvatarCachePath, ignored);
            shellState->aiAvatarCachePath.clear();
        }
    }

    if (shellState->aiAvatarRunning &&
        shellState->aiAvatarFuture.valid() &&
        shellState->aiAvatarFuture.wait_for(
            std::chrono::seconds(0)) ==
            std::future_status::ready) {
        jcut::ai::RemoteImageCore downloaded =
            shellState->aiAvatarFuture.get();
        shellState->aiAvatarRunning = false;
        if (downloaded.url == desiredUrl && downloaded.ok) {
            const fs::path cachePath =
                fs::temp_directory_path() /
                ("jcut-imgui-avatar-" +
                 std::to_string(
                     static_cast<unsigned long long>(::getpid())) +
                 "-" +
                 std::to_string(
                     std::hash<std::string>{}(downloaded.url)) +
                 ".image");
            const fs::path partialPath =
                cachePath.string() + ".part";
            std::ofstream output(
                partialPath,
                std::ios::binary | std::ios::trunc);
            output.write(
                reinterpret_cast<const char*>(
                    downloaded.bytes.data()),
                static_cast<std::streamsize>(
                    downloaded.bytes.size()));
            output.close();
            std::error_code fileError;
            if (!output || !fs::exists(partialPath, fileError)) {
                shellState->aiAvatarError =
                    "Profile avatar cache write failed.";
                fs::remove(partialPath, fileError);
            } else {
                fs::rename(partialPath, cachePath, fileError);
                if (fileError) {
                    shellState->aiAvatarError =
                        "Profile avatar cache commit failed: " +
                        fileError.message();
                    fs::remove(partialPath, fileError);
                } else {
                    jcut::DecoderPolicySettingsCore policy =
                        shellState->previewDecoderPolicy;
                    policy.decodePreference =
                        jcut::DecodePreferenceCore::Software;
                    const auto decoded =
                        jcut::standalone_render::
                            decodeStandaloneMediaFrame(
                                cachePath.string(),
                                0,
                                {96, 96},
                                policy);
                    if (!decoded.success ||
                        decoded.image.empty()) {
                        shellState->aiAvatarError =
                            decoded.message.empty()
                            ? "Profile avatar image could not be decoded."
                            : decoded.message;
                        fs::remove(cachePath, fileError);
                    } else {
                        ImTextureID texture = 0;
                        std::string uploadError;
                        if (vulkanShell->uploadAuxiliaryImage(
                                decoded.image,
                                &vulkanShell->
                                    aiProfileAvatarTexture,
                                &texture,
                                &uploadError)) {
                            shellState->aiAvatarTextureId =
                                texture;
                            shellState->aiAvatarSize =
                                decoded.image.size;
                            shellState->aiAvatarLoadedUrl =
                                downloaded.url;
                            shellState->aiAvatarCachePath =
                                cachePath.string();
                            shellState->aiAvatarError.clear();
                        } else {
                            shellState->aiAvatarError =
                                uploadError.empty()
                                ? "Profile avatar upload failed."
                                : std::move(uploadError);
                            fs::remove(cachePath, fileError);
                        }
                    }
                }
            }
        } else if (downloaded.url == desiredUrl) {
            shellState->aiAvatarError =
                downloaded.error.message.empty()
                ? "Profile avatar download failed."
                : downloaded.error.message;
        }
    }

    if (desiredUrl.empty()) {
        if (!shellState->aiAvatarRunning) {
            shellState->aiAvatarRequestedUrl.clear();
            shellState->aiAvatarError.clear();
        }
        return;
    }
    if (shellState->aiAvatarRunning ||
        shellState->aiAvatarLoadedUrl == desiredUrl ||
        shellState->aiAvatarRequestedUrl == desiredUrl) {
        return;
    }
    shellState->aiAvatarRequestedUrl = desiredUrl;
    shellState->aiAvatarError.clear();
    shellState->aiAvatarRunning = true;
    shellState->aiAvatarFuture = std::async(
        std::launch::async,
        [desiredUrl]() {
            return jcut::ai::downloadRemoteImageCore(
                desiredUrl, 10000, 4u * 1024u * 1024u);
        });
}

jcut::standalone_render::StandaloneDecodedFrameResult
decodeFaceAvatarStrip(
    const std::string& sourcePath,
    const std::vector<jcut::FaceContinuityTrackCore>& tracks,
    jcut::DecoderPolicySettingsCore policy,
    int avatarSize,
    int gapPixels)
{
    jcut::standalone_render::StandaloneDecodedFrameResult result;
    if (sourcePath.empty() || tracks.empty()) {
        result.message = "Face reference source or tracks are empty.";
        return result;
    }
    policy.decodePreference = jcut::DecodePreferenceCore::Software;
    jcut::core::SizeI decodeSize{1280, 720};
    const auto info =
        jcut::standalone_render::probeStandaloneMedia(sourcePath);
    if (info.frameSize.valid()) {
        const double scale = std::min(
            1.0,
            std::min(
                1280.0 / info.frameSize.width,
                720.0 / info.frameSize.height));
        decodeSize = {
            std::max(
                1,
                static_cast<int>(std::lround(
                    info.frameSize.width * scale))),
            std::max(
                1,
                static_cast<int>(std::lround(
                    info.frameSize.height * scale)))};
    }
    std::vector<jcut::core::ImageBuffer> avatars;
    avatars.reserve(tracks.size());
    for (const auto& track : tracks) {
        auto decoded =
            jcut::standalone_render::decodeStandaloneMediaFrame(
                sourcePath,
                static_cast<int>(
                    std::clamp<std::int64_t>(
                        track.firstFrame,
                        0,
                        std::numeric_limits<int>::max())),
                decodeSize,
                policy);
        if (!decoded.success || decoded.image.empty()) {
            result.message = decoded.message.empty()
                ? "Face reference could not be decoded."
                : decoded.message;
            return result;
        }
        jcut::core::ImageBuffer avatar =
            jcut::cropFaceAvatarImageCore(
                decoded.image,
                track.x,
                track.y,
                track.box,
                avatarSize);
        if (avatar.empty()) {
            result.message = "Face reference crop is empty.";
            return result;
        }
        avatars.push_back(std::move(avatar));
    }
    result.image =
        jcut::faceAvatarStripImageCore(avatars, gapPixels);
    result.success = !result.image.empty();
    if (!result.success) {
        result.message = "Face reference strip is empty.";
    }
    return result;
}

void refreshFaceReferenceTexture(
    ShellState* shellState,
    VulkanShell* vulkanShell)
{
    if (!shellState || !vulkanShell) return;
    if (!shellState->faceReferenceLoadedKey.empty() &&
        shellState->faceReferenceLoadedKey !=
            shellState->faceReferenceDesiredKey) {
        vulkanShell->releaseAuxiliaryTexture(
            &vulkanShell->faceReferenceTexture);
        shellState->faceReferenceTextureId = 0;
        shellState->faceReferenceSize = {};
        shellState->faceReferenceLoadedKey.clear();
    }
    if (shellState->faceReferenceRunning &&
        shellState->faceReferenceFuture.valid() &&
        shellState->faceReferenceFuture.wait_for(
            std::chrono::seconds(0)) ==
            std::future_status::ready) {
        const auto decoded =
            shellState->faceReferenceFuture.get();
        shellState->faceReferenceRunning = false;
        if (shellState->faceReferencePendingKey ==
            shellState->faceReferenceDesiredKey) {
            if (!decoded.success || decoded.image.empty()) {
                shellState->faceReferenceError =
                    decoded.message.empty()
                    ? "Face reference could not be decoded."
                    : decoded.message;
            } else {
                ImTextureID texture = 0;
                std::string uploadError;
                if (vulkanShell->uploadAuxiliaryImage(
                        decoded.image,
                        &vulkanShell->faceReferenceTexture,
                        &texture,
                        &uploadError)) {
                    shellState->faceReferenceTextureId = texture;
                    shellState->faceReferenceSize =
                        decoded.image.size;
                    shellState->faceReferenceLoadedKey =
                        shellState->faceReferencePendingKey;
                    shellState->faceReferenceError.clear();
                } else {
                    shellState->faceReferenceError =
                        uploadError.empty()
                        ? "Face reference upload failed."
                        : std::move(uploadError);
                }
            }
        }
    }
    if (shellState->faceReferenceDesiredKey.empty()) {
        if (!shellState->faceReferenceRunning) {
            shellState->faceReferencePendingKey.clear();
            shellState->faceReferenceError.clear();
        }
        return;
    }
    if (shellState->faceReferenceRunning ||
        shellState->faceReferenceLoadedKey ==
            shellState->faceReferenceDesiredKey ||
        shellState->faceReferencePendingKey ==
            shellState->faceReferenceDesiredKey) {
        return;
    }
    const std::string key =
        shellState->faceReferenceDesiredKey;
    const std::string sourcePath =
        shellState->faceReferenceSourcePath;
    const std::vector<jcut::FaceContinuityTrackCore> tracks =
        shellState->faceReferenceTracks;
    const jcut::DecoderPolicySettingsCore policy =
        shellState->previewDecoderPolicy;
    shellState->faceReferencePendingKey = key;
    shellState->faceReferenceRunning = true;
    shellState->faceReferenceError.clear();
    shellState->faceReferenceFuture = std::async(
        std::launch::async,
        [sourcePath, tracks, policy]() {
            return decodeFaceAvatarStrip(
                sourcePath, tracks, policy, 160, 4);
        });
}

void refreshSectionAvatarTexture(
    ShellState* shellState,
    VulkanShell* vulkanShell)
{
    if (!shellState || !vulkanShell) return;
    if (!shellState->sectionAvatarLoadedKey.empty() &&
        shellState->sectionAvatarLoadedKey !=
            shellState->sectionAvatarDesiredKey) {
        vulkanShell->releaseAuxiliaryTexture(
            &vulkanShell->sectionAvatarTexture);
        shellState->sectionAvatarTextureId = 0;
        shellState->sectionAvatarSize = {};
        shellState->sectionAvatarLoadedKey.clear();
    }
    if (shellState->sectionAvatarRunning &&
        shellState->sectionAvatarFuture.valid() &&
        shellState->sectionAvatarFuture.wait_for(
            std::chrono::seconds(0)) ==
            std::future_status::ready) {
        const auto decoded =
            shellState->sectionAvatarFuture.get();
        shellState->sectionAvatarRunning = false;
        if (shellState->sectionAvatarPendingKey ==
            shellState->sectionAvatarDesiredKey) {
            if (!decoded.success || decoded.image.empty()) {
                shellState->sectionAvatarError =
                    decoded.message.empty()
                    ? "Section avatars could not be decoded."
                    : decoded.message;
            } else {
                ImTextureID texture = 0;
                std::string uploadError;
                if (vulkanShell->uploadAuxiliaryImage(
                        decoded.image,
                        &vulkanShell->sectionAvatarTexture,
                        &texture,
                        &uploadError)) {
                    shellState->sectionAvatarTextureId = texture;
                    shellState->sectionAvatarSize =
                        decoded.image.size;
                    shellState->sectionAvatarLoadedKey =
                        shellState->sectionAvatarPendingKey;
                    shellState->sectionAvatarError.clear();
                } else {
                    shellState->sectionAvatarError =
                        uploadError.empty()
                        ? "Section avatar upload failed."
                        : std::move(uploadError);
                }
            }
        }
    }
    if (shellState->sectionAvatarDesiredKey.empty()) {
        if (!shellState->sectionAvatarRunning) {
            shellState->sectionAvatarPendingKey.clear();
            shellState->sectionAvatarError.clear();
        }
        return;
    }
    if (shellState->sectionAvatarRunning ||
        shellState->sectionAvatarLoadedKey ==
            shellState->sectionAvatarDesiredKey ||
        shellState->sectionAvatarPendingKey ==
            shellState->sectionAvatarDesiredKey) {
        return;
    }
    const std::string key =
        shellState->sectionAvatarDesiredKey;
    const std::string sourcePath =
        shellState->sectionAvatarSourcePath;
    const std::vector<jcut::FaceContinuityTrackCore> tracks =
        shellState->sectionAvatarTracks;
    const jcut::DecoderPolicySettingsCore policy =
        shellState->previewDecoderPolicy;
    shellState->sectionAvatarPendingKey = key;
    shellState->sectionAvatarRunning = true;
    shellState->sectionAvatarError.clear();
    shellState->sectionAvatarFuture = std::async(
        std::launch::async,
        [sourcePath, tracks, policy]() {
            return decodeFaceAvatarStrip(
                sourcePath, tracks, policy, 80, 2);
        });
}

void removeInspectorKeyframe(
    ShellState* shellState,
    int clipId,
    jcut::EditorKeyframeChannel channel,
    std::int64_t frame);

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

    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_B)) {
        if (selectedClipsCanSplitAtFrame(
                snapshot, snapshot.transport.currentFrame)) {
            applyCommand(
                shellState,
                jcut::SplitSelectedClipsCommand{
                snapshot.transport.currentFrame});
        } else {
            shellState->statusMessage =
                "split unavailable: no selected clip intersects playhead";
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
    const bool deletePressed =
        ImGui::IsKeyChordPressed(ImGuiKey_Delete);
    const bool rowBackspacePressed =
        ImGui::IsKeyChordPressed(ImGuiKey_Backspace);
    if (deletePressed || rowBackspacePressed) {
        InspectorDeleteTarget& target =
            shellState->inspectorDeleteTarget;
        const bool focusedInspectorTarget =
            target.kind != InspectorDeleteTargetKind::None &&
            target.documentGeneration == shellState->documentGeneration &&
            target.focusedUiFrame + 1 == shellState->uiFrameCounter;
        if (focusedInspectorTarget) {
            if (target.kind ==
                InspectorDeleteTargetKind::TitleKeyframe) {
                applyCommand(
                    shellState,
                    jcut::RemoveTitleKeyframeCommand{
                        target.clipId, target.frame});
            } else if (
                target.kind ==
                InspectorDeleteTargetKind::SyncMarker) {
                applyCommand(
                    shellState,
                    jcut::RemoveRenderSyncMarkerCommand{
                        target.markerClipId,
                        target.frame,
                        target.markerSkipFrame});
            } else if (
                target.kind ==
                InspectorDeleteTargetKind::TranscriptWord) {
                const TranscriptInspectorCache& cache =
                    shellState->transcriptCache;
                if (cache.clipId == target.clipId &&
                    cache.session.activeCutMutable &&
                    cache.session.activePath ==
                        target.transcriptPath &&
                    cache.selectionDraftValid &&
                    cache.selectedWord.segmentIndex ==
                        target.transcriptSegmentIndex &&
                    cache.selectedWord.wordIndex ==
                        target.transcriptWordIndex) {
                    shellState->transcriptDeletePopupRequested =
                        true;
                } else {
                    shellState->statusMessage =
                        "transcript delete canceled after selection change";
                }
            } else {
                removeInspectorKeyframe(
                    shellState,
                    target.clipId,
                    target.channel,
                    target.frame);
            }
            target = {};
        } else if (deletePressed) {
            // Qt reserves unmodified Backspace for focused row/table removal;
            // only Delete falls through to timeline clip deletion.
            deleteSelectedClips(shellState);
        }
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
    const bool canSplit = selectedClipsCanSplitAtFrame(
        snapshot, snapshot.transport.currentFrame);
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
        if (ImGui::MenuItem(
                selectionCount > 1
                    ? "Split Selected At Playhead"
                    : "Split At Playhead",
                "Ctrl+B",
                false,
                canSplit)) {
            applyCommand(
                shellState,
                jcut::SplitSelectedClipsCommand{
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
                    if (ImGui::BeginPopupContextItem(
                            "FilesystemMediaContext")) {
                        const fs::path desktopPath =
                            isDir && !isSequence
                                ? entryPath
                                : entryPath.parent_path();
                        if (ImGui::MenuItem(
                                isDir && !isSequence
                                    ? "Open Folder"
                                    : "Open Containing Folder")) {
                            std::string openError;
                            if (openDesktopPath(
                                    desktopPath, &openError)) {
                                shellState->statusMessage =
                                    "opened " + pathString(desktopPath);
                            } else {
                                shellState->statusMessage =
                                    std::move(openError);
                            }
                        }
                        ImGui::Separator();
                        if (ImGui::MenuItem("Copy Absolute Path")) {
                            ImGui::SetClipboardText(
                                entryPathText.c_str());
                            shellState->statusMessage =
                                "absolute media path copied";
                        }
                        ImGui::EndPopup();
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
                if (shellState->mediaThumbnailTextureId != 0 &&
                    shellState->mediaThumbnailSize.valid() &&
                    shellState->mediaThumbnailLoadedPath ==
                        previewPath) {
                    const float availableWidth =
                        std::max(
                            1.0f,
                            ImGui::GetContentRegionAvail().x);
                    const float scale = std::min(
                        1.0f,
                        std::min(
                            availableWidth /
                                static_cast<float>(
                                    shellState
                                        ->mediaThumbnailSize.width),
                            180.0f /
                                static_cast<float>(
                                    shellState
                                        ->mediaThumbnailSize.height)));
                    ImGui::Image(
                        shellState->mediaThumbnailTextureId,
                        ImVec2(
                            shellState->mediaThumbnailSize.width *
                                scale,
                            shellState->mediaThumbnailSize.height *
                                scale));
                } else if (
                    shellState->mediaThumbnailRunning &&
                    shellState->mediaThumbnailPendingPath ==
                        previewPath) {
                    ImGui::TextDisabled(
                        "Loading thumbnail...");
                } else if (
                    !shellState->mediaThumbnailError.empty() &&
                    shellState->mediaThumbnailPendingPath ==
                        previewPath) {
                    ImGui::TextDisabled(
                        "%s",
                        shellState->mediaThumbnailError.c_str());
                }
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
    const bool videoPreviewMode =
        snapshot.transport.previewViewMode != "audio";
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
    const bool mouseInsideCanvas = videoPreviewMode &&
        ImGui::IsMouseHoveringRect(canvasPos, canvasMax);
    const jcut::EditorClip* previewTitleClip = selectedClip(snapshot);
    const bool selectedTitleIsActive = videoPreviewMode && previewTitleClip &&
        previewTitleClip->mediaKind == "title" &&
        snapshot.transport.currentFrame >= previewTitleClip->startFrame &&
        snapshot.transport.currentFrame <
            previewTitleClip->startFrame + std::max(1, previewTitleClip->durationFrames);
    const bool selectedTransformClipIsActive = videoPreviewMode && previewTitleClip &&
        previewTitleClip->mediaKind != "audio" &&
        previewTitleClip->mediaKind != "title" &&
        snapshot.transport.currentFrame >= previewTitleClip->startFrame &&
        snapshot.transport.currentFrame <
            previewTitleClip->startFrame + std::max(1, previewTitleClip->durationFrames);
    const jcut::EditorClip* correctionClip = previewTitleClip;
    const bool correctionClipIsActive = videoPreviewMode && correctionClip &&
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
    ImVec2 transformRotationHandleMin{};
    ImVec2 transformRotationHandleMax{};
    ImVec2 transformRotationHandleCenter{};
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
        transformRotationHandleCenter = {
            std::clamp(
                center.x,
                frameMin.x + kHandleRadius,
                frameMax.x - kHandleRadius),
            transformBoundsMin.y - 26.0f};
        if (transformRotationHandleCenter.y - kHandleRadius < frameMin.y) {
            transformRotationHandleCenter.y =
                transformBoundsMin.y + 26.0f;
        }
        transformRotationHandleCenter.y = std::clamp(
            transformRotationHandleCenter.y,
            frameMin.y + kHandleRadius,
            frameMax.y - kHandleRadius);
        transformRotationHandleMin = {
            transformRotationHandleCenter.x - kHandleRadius,
            transformRotationHandleCenter.y - kHandleRadius};
        transformRotationHandleMax = {
            transformRotationHandleCenter.x + kHandleRadius,
            transformRotationHandleCenter.y + kHandleRadius};
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
        if (pointInRect(
                io.MousePos,
                transformRotationHandleMin,
                transformRotationHandleMax)) {
            dragMode = PreviewTransformDragMode::Rotate;
        } else if (pointInRect(io.MousePos, transformCornerHandleMin, transformCornerHandleMax)) {
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
            } else if (
                shellState->previewTransformDragMode ==
                PreviewTransformDragMode::Rotate) {
                const jcut::preview::PointD center{
                    (shellState->previewTransformDragOriginBoundsMin.x +
                     shellState->previewTransformDragOriginBoundsMax.x) *
                        0.5,
                    (shellState->previewTransformDragOriginBoundsMin.y +
                     shellState->previewTransformDragOriginBoundsMax.y) *
                        0.5};
                next.rotation = jcut::preview::rotationForPointerDrag(
                    next.rotation,
                    center,
                    {shellState->previewTransformDragOriginMouse.x,
                     shellState->previewTransformDragOriginMouse.y},
                    {io.MousePos.x, io.MousePos.y},
                    io.KeyShift ? 15.0 : 0.0);
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
    drawList->AddRectFilled(
        frameMin,
        frameMax,
        videoPreviewMode
            ? IM_COL32(12, 14, 18, 255)
            : IM_COL32(42, 46, 54, 255),
        4.0f);
    if (videoPreviewMode && shellState->previewTextureId != 0) {
        if (shellState->
                previewHardwarePresentationTransformValid) {
            const jcut::EditorTransformKeyframe& transform =
                shellState->
                    previewHardwarePresentationTransform;
            const jcut::preview::RectD outputRect{
                frameMin.x, frameMin.y, frameWidth, frameHeight};
            const jcut::preview::PointD outputSize{
                static_cast<double>(std::max(
                    1, snapshot.exportRequest.outputSize.width)),
                static_cast<double>(std::max(
                    1, snapshot.exportRequest.outputSize.height))};
            const jcut::preview::RectD imageRect =
                jcut::preview::fittedPresentationRect(
                    outputRect,
                    outputSize,
                    {
                        static_cast<double>(std::max(
                            1,
                            shellState->
                                previewHardwareSourceSize.width)),
                        static_cast<double>(std::max(
                            1,
                            shellState->
                                previewHardwareSourceSize.height))});
            const auto quad =
                jcut::preview::transformedPresentationQuad(
                    imageRect,
                    outputRect,
                    outputSize,
                    {
                        transform.translationX,
                        transform.translationY},
                    {transform.scaleX, transform.scaleY},
                    transform.rotation);
            const auto screenPoint = [](const auto& point) {
                return ImVec2(
                    static_cast<float>(point.x),
                    static_cast<float>(point.y));
            };
            const ImU32 tint = IM_COL32(
                255, 255, 255,
                static_cast<int>(std::lround(
                    shellState->
                        previewHardwarePresentationOpacity *
                    255.0)));
            drawList->AddImageQuad(
                shellState->previewTextureId,
                screenPoint(quad[0]),
                screenPoint(quad[1]),
                screenPoint(quad[2]),
                screenPoint(quad[3]),
                ImVec2(0.0f, 0.0f),
                ImVec2(1.0f, 0.0f),
                ImVec2(1.0f, 1.0f),
                ImVec2(0.0f, 1.0f),
                tint);
        } else {
            drawList->AddImage(
                shellState->previewTextureId,
                frameMin,
                frameMax,
                ImVec2(0.0f, 0.0f),
                ImVec2(1.0f, 1.0f));
        }
        if (shellState->previewOverlayTextureId != 0) {
            const float outputWidth = static_cast<float>(
                std::max(
                    1,
                    snapshot.exportRequest.outputSize.width));
            const float outputHeight = static_cast<float>(
                std::max(
                    1,
                    snapshot.exportRequest.outputSize.height));
            const ImVec2 overlayMin{
                frameMin.x +
                    frameWidth *
                    static_cast<float>(
                        shellState->previewOverlayX) /
                    outputWidth,
                frameMin.y +
                    frameHeight *
                    static_cast<float>(
                        shellState->previewOverlayY) /
                    outputHeight};
            const ImVec2 overlayMax{
                overlayMin.x +
                    frameWidth *
                    static_cast<float>(
                        shellState->
                            previewOverlaySize.width) /
                    outputWidth,
                overlayMin.y +
                    frameHeight *
                    static_cast<float>(
                        shellState->
                            previewOverlaySize.height) /
                    outputHeight};
            drawList->AddImage(
                shellState->previewOverlayTextureId,
                overlayMin,
                overlayMax,
                ImVec2(0.0f, 0.0f),
                ImVec2(1.0f, 1.0f));
        }
    } else if (!videoPreviewMode) {
        const jcut::ImGuiAudioStatus audioStatus =
            shellState->audioRuntime.status();
        const float waveformCenterY =
            (frameMin.y + frameMax.y) * 0.5f;
        const float waveformHalfHeight =
            frameHeight * 0.38f;
        drawList->AddLine(
            ImVec2(frameMin.x + 10.0f, waveformCenterY),
            ImVec2(frameMax.x - 10.0f, waveformCenterY),
            IM_COL32(72, 82, 94, 255));
        for (std::size_t point = 0;
             point < audioStatus.recentWaveform.size();
             ++point) {
            const float x = frameMin.x + 10.0f +
                (frameWidth - 20.0f) *
                    static_cast<float>(point) /
                    static_cast<float>(
                        audioStatus.recentWaveform.size() - 1);
            const float amplitude =
                audioStatus.recentWaveform[point] *
                waveformHalfHeight;
            drawList->AddLine(
                ImVec2(x, waveformCenterY - amplitude),
                ImVec2(x, waveformCenterY + amplitude),
                IM_COL32(92, 210, 178, 230));
        }
    }
    drawList->AddRect(frameMin, frameMax, IM_COL32(160, 110, 56, 255), 4.0f, 0, 2.0f);
    const float inset = std::clamp(0.12f / std::max(0.5f, zoom), 0.04f, 0.18f);
    const ImVec2 safeMin(frameMin.x + frameWidth * inset, frameMin.y + frameHeight * 0.08f);
    const ImVec2 safeMax(frameMax.x - frameWidth * inset, frameMax.y - frameHeight * 0.08f);
    drawList->AddRect(safeMin, safeMax, IM_COL32(236, 160, 74, 255), 4.0f, 0, 2.0f);
    drawList->AddText(ImVec2(frameMin.x + 14.0f, frameMin.y + 12.0f),
                      IM_COL32(242, 242, 242, 255),
                      videoPreviewMode ? "Program" : "Audio Preview");
    std::string previewDetail =
        "Frame " + std::to_string(snapshot.transport.currentFrame);
    if (!videoPreviewMode) {
        const jcut::ImGuiAudioStatus audioStatus =
            shellState->audioRuntime.status();
        previewDetail += " | " + audioStatus.message;
    }
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
        drawList->AddLine(
            ImVec2(
                (transformBoundsMin.x + transformBoundsMax.x) * 0.5f,
                transformBoundsMin.y),
            transformRotationHandleCenter,
            outlineColor,
            2.0f);
        drawList->AddCircleFilled(
            transformRotationHandleCenter,
            7.0f,
            IM_COL32(24, 28, 34, 245));
        drawList->AddCircle(
            transformRotationHandleCenter,
            7.0f,
            outlineColor,
            0,
            2.0f);
        if (mouseInsideProgram) {
            if (pointInRect(
                    io.MousePos,
                    transformRotationHandleMin,
                    transformRotationHandleMax)) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            } else if (pointInRect(io.MousePos, transformCornerHandleMin, transformCornerHandleMax)) {
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
    const int transportEndFrame = [&]() {
        int endFrame = 0;
        for (const jcut::EditorClip& clip : snapshot.clips) {
            endFrame = std::max(
                endFrame, clip.startFrame + clip.durationFrames);
        }
        return endFrame;
    }();
    if (ImGui::Button("|<")) {
        applyCommand(shellState, jcut::SeekToFrameCommand{0});
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Go to start");
    }
    ImGui::SameLine();
    if (ImGui::ArrowButton("StepBack", ImGuiDir_Left)) {
        applyCommand(shellState, jcut::StepFrameCommand{-1});
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Previous playable frame");
    }
    ImGui::SameLine();
    if (ImGui::Button(snapshot.transport.playbackActive ? "Pause" : "Play")) {
        applyCommand(shellState, jcut::TogglePlaybackCommand{});
    }
    ImGui::SameLine();
    if (ImGui::ArrowButton("StepForward", ImGuiDir_Right)) {
        applyCommand(shellState, jcut::StepFrameCommand{1});
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Next playable frame");
    }
    ImGui::SameLine();
    if (ImGui::Button(">|")) {
        applyCommand(
            shellState, jcut::SeekToFrameCommand{transportEndFrame});
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Go to end");
    }
    constexpr std::array<float, 9> transportSpeeds = {
        0.1f, 0.25f, 0.5f, 0.75f, 1.0f,
        1.25f, 1.5f, 2.0f, 3.0f};
    constexpr std::array<const char*, 9> transportSpeedLabels = {
        "10%", "25%", "50%", "75%", "100%",
        "125%", "150%", "200%", "300%"};
    int speedIndex = 0;
    float nearestSpeedDistance =
        std::numeric_limits<float>::max();
    for (int index = 0;
         index < static_cast<int>(transportSpeeds.size());
         ++index) {
        const float distance = std::abs(
            snapshot.transport.playbackSpeed -
            transportSpeeds[static_cast<std::size_t>(index)]);
        if (distance < nearestSpeedDistance) {
            speedIndex = index;
            nearestSpeedDistance = distance;
        }
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(76.0f);
    if (ImGui::Combo(
            "Speed",
            &speedIndex,
            transportSpeedLabels.data(),
            static_cast<int>(transportSpeedLabels.size()))) {
        applyCommand(
            shellState,
            jcut::SetPlaybackSpeedCommand{
                transportSpeeds[static_cast<std::size_t>(speedIndex)]});
    }
    bool playbackLoopEnabled =
        snapshot.transport.playbackLoopEnabled;
    ImGui::SameLine();
    if (ImGui::Checkbox("Loop", &playbackLoopEnabled)) {
        applyCommand(
            shellState,
            jcut::SetPlaybackLoopEnabledCommand{
                playbackLoopEnabled});
    }
    constexpr std::array<const char*, 2> previewModeLabels = {
        "Video", "Audio"};
    int previewModeIndex = videoPreviewMode ? 0 : 1;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(82.0f);
    if (ImGui::Combo(
            "View",
            &previewModeIndex,
            previewModeLabels.data(),
            static_cast<int>(previewModeLabels.size()))) {
        applyCommand(
            shellState,
            jcut::SetPreviewViewModeCommand{
                previewModeIndex == 1 ? "audio" : "video"});
    }
    bool transportAudioMuted =
        snapshot.transport.audioMuted;
    ImGui::SameLine();
    if (ImGui::Checkbox("Mute", &transportAudioMuted)) {
        applyCommand(
            shellState,
            jcut::SetTransportAudioCommand{
                transportAudioMuted,
                snapshot.transport.audioVolume});
    }
    float transportAudioVolume =
        snapshot.transport.audioVolume;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100.0f);
    if (ImGui::SliderFloat(
            "Volume",
            &transportAudioVolume,
            0.0f,
            1.0f,
            "%.2f",
            ImGuiSliderFlags_None)) {
        applyCommand(
            shellState,
            jcut::SetTransportAudioCommand{
                snapshot.transport.audioMuted,
                transportAudioVolume});
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
    int hoveredTrackVisualToggleId = 0;
    int hoveredTrackAudioToggleId = 0;
    bool hoveredTrackVisualToggleAvailable = false;
    bool hoveredTrackAudioToggleAvailable = false;
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
        const jcut::EditorTrackMediaPresenceCore trackPresence =
            jcut::editorTrackMediaPresenceCore(snapshot, track.id);
        const ImVec2 visualToggleMin(
            origin.x + kTimelineLabelWidth - 44.0f, y + 4.0f);
        const ImVec2 visualToggleMax(
            origin.x + kTimelineLabelWidth - 27.0f,
            y + kTimelineClipHeight - 4.0f);
        const ImVec2 audioToggleMin(
            origin.x + kTimelineLabelWidth - 23.0f, y + 4.0f);
        const ImVec2 audioToggleMax(
            origin.x + kTimelineLabelWidth - 6.0f,
            y + kTimelineClipHeight - 4.0f);
        const ImU32 disabledToggleColor = IM_COL32(66, 70, 76, 255);
        const ImU32 visualToggleColor = !trackPresence.hasVisual
            ? disabledToggleColor
            : (track.visualMode == 2
                   ? IM_COL32(115, 72, 72, 255)
                   : (track.visualMode == 1
                          ? IM_COL32(191, 145, 66, 255)
                          : IM_COL32(70, 143, 102, 255)));
        const ImU32 audioToggleColor =
            !trackPresence.hasAudio || generatedChildTrack
            ? disabledToggleColor
            : (track.audioEnabled
                   ? IM_COL32(70, 143, 102, 255)
                   : IM_COL32(115, 72, 72, 255));
        drawList->AddRectFilled(
            visualToggleMin, visualToggleMax, visualToggleColor, 3.0f);
        drawList->AddRectFilled(
            audioToggleMin, audioToggleMax, audioToggleColor, 3.0f);
        drawList->AddText(
            ImVec2(visualToggleMin.x + 4.0f, visualToggleMin.y + 1.0f),
            IM_COL32(238, 240, 242, 255),
            "V");
        drawList->AddText(
            ImVec2(audioToggleMin.x + 4.0f, audioToggleMin.y + 1.0f),
            IM_COL32(238, 240, 242, 255),
            "A");
        if (ImGui::IsMouseHoveringRect(
                visualToggleMin, visualToggleMax)) {
            hoveredTrackVisualToggleId = track.id;
            hoveredTrackVisualToggleAvailable = trackPresence.hasVisual;
        }
        if (ImGui::IsMouseHoveringRect(audioToggleMin, audioToggleMax)) {
            hoveredTrackAudioToggleId = track.id;
            hoveredTrackAudioToggleAvailable =
                trackPresence.hasAudio && !generatedChildTrack;
        }
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
            if (snapshot.panels.showWaveform &&
                track.audioWaveformVisible &&
                clip.hasAudio &&
                clip.audioEnabled) {
                const int waveformColumns = std::clamp(
                    static_cast<int>(
                        std::floor(clipWidth - 4.0f)),
                    16,
                    512);
                std::vector<float> waveformMinimum;
                std::vector<float> waveformMaximum;
                if (shellState->audioRuntime.queryClipWaveform(
                        clip.id,
                        waveformColumns,
                        &waveformMinimum,
                        &waveformMaximum)) {
                    const float waveformCenter =
                        (clipMin.y + clipMax.y) * 0.5f;
                    const float waveformRadius =
                        std::max(
                            1.0f,
                            (clipMax.y - clipMin.y) *
                                0.38f);
                    const float waveformSpan =
                        std::max(
                            1.0f,
                            clipMax.x - clipMin.x - 4.0f);
                    const ImU32 waveformColor =
                        clip.selected
                        ? IM_COL32(70, 66, 48, 205)
                        : IM_COL32(222, 237, 245, 190);
                    for (int column = 0;
                         column < waveformColumns;
                         ++column) {
                        const float x =
                            clipMin.x + 2.0f +
                            (static_cast<float>(column) +
                             0.5f) *
                                waveformSpan /
                                waveformColumns;
                        const float minimum =
                            waveformMinimum[
                                static_cast<std::size_t>(
                                    column)];
                        const float maximum =
                            waveformMaximum[
                                static_cast<std::size_t>(
                                    column)];
                        drawList->AddLine(
                            ImVec2(
                                x,
                                waveformCenter -
                                    maximum *
                                        waveformRadius),
                            ImVec2(
                                x,
                                waveformCenter -
                                    minimum *
                                        waveformRadius),
                            waveformColor,
                            1.0f);
                    }
                }
            }
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
    if (hoveredTrackVisualToggleId != 0) {
        ImGui::SetTooltip(
            hoveredTrackVisualToggleAvailable
                ? "Cycle track visual mode: Enabled / Force Opaque / Hidden"
                : "No visual clips on this track");
    } else if (hoveredTrackAudioToggleId != 0) {
        ImGui::SetTooltip(
            hoveredTrackAudioToggleAvailable
                ? "Toggle track audio"
                : "No audio clips on this track");
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
        if (hoveredTrackVisualToggleId != 0) {
            if (hoveredTrackVisualToggleAvailable) {
                const auto trackIt = std::find_if(
                    snapshot.tracks.begin(), snapshot.tracks.end(),
                    [&](const jcut::EditorTrack& track) {
                        return track.id == hoveredTrackVisualToggleId;
                    });
                if (trackIt != snapshot.tracks.end()) {
                    applyCommand(shellState, jcut::SetTrackStateCommand{
                        trackIt->id,
                        (std::clamp(trackIt->visualMode, 0, 2) + 1) % 3,
                        trackIt->audioEnabled,
                        trackIt->audioGain,
                        trackIt->audioMuted,
                        trackIt->audioSolo,
                        trackIt->gradingPreviewEnabled,
                    });
                }
            }
            clearTimelineDrag(shellState);
        } else if (hoveredTrackAudioToggleId != 0) {
            if (hoveredTrackAudioToggleAvailable) {
                const auto trackIt = std::find_if(
                    snapshot.tracks.begin(), snapshot.tracks.end(),
                    [&](const jcut::EditorTrack& track) {
                        return track.id == hoveredTrackAudioToggleId;
                    });
                if (trackIt != snapshot.tracks.end()) {
                    applyCommand(shellState, jcut::SetTrackStateCommand{
                        trackIt->id,
                        trackIt->visualMode,
                        !trackIt->audioEnabled,
                        trackIt->audioGain,
                        trackIt->audioMuted,
                        trackIt->audioSolo,
                        trackIt->gradingPreviewEnabled,
                    });
                }
            }
            clearTimelineDrag(shellState);
        } else if (hoveredClipId != 0) {
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
               mouseInsideCanvas) {
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
        shellState->timelineContextClickFrame =
            frameFromTimelineX(origin.x, mousePos.x);
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
        const bool contextDocumentValid =
            shellState->timelineContextDocumentGeneration ==
            shellState->documentGeneration;
        const bool contextClipValid =
            shellState->timelineContextClipId == 0 ||
            contextClip != nullptr;
        if (!contextDocumentValid || !contextClipValid) {
            shellState->statusMessage =
                "timeline menu closed after document change";
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::BeginMenu("Export Range")) {
            const std::int64_t playhead =
                snapshot.transport.currentFrame;
            if (ImGui::MenuItem("Set Start At Playhead")) {
                applyCommand(
                    shellState,
                    jcut::EditExportRangesCommand{
                        jcut::ExportRangeEdit::SetStartAtPlayhead,
                        playhead});
            }
            if (ImGui::MenuItem("Set End At Playhead")) {
                applyCommand(
                    shellState,
                    jcut::EditExportRangesCommand{
                        jcut::ExportRangeEdit::SetEndAtPlayhead,
                        playhead});
            }
            const bool canSplitExportRange =
                jcut::export_range::canSplitAt(
                    snapshot.exportRanges, playhead);
            if (ImGui::MenuItem(
                    "Split At Playhead",
                    nullptr,
                    false,
                    canSplitExportRange)) {
                applyCommand(
                    shellState,
                    jcut::EditExportRangesCommand{
                        jcut::ExportRangeEdit::SplitAtPlayhead,
                        playhead});
            }
            if (ImGui::MenuItem("Reset")) {
                applyCommand(
                    shellState,
                    jcut::EditExportRangesCommand{
                        jcut::ExportRangeEdit::Reset,
                        playhead});
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Create Title")) {
            const jcut::CommandResult result = applyCommand(
                shellState,
                jcut::CreateTitleClipCommand{
                    shellState->timelineContextClickFrame,
                    jcut::kEditorDefaultTitleDurationFrames});
            if (result.applied) {
                shellState->titleDraftClipId = -1;
            }
        }
        ImGui::Separator();
        ImGui::BeginDisabled(contextClip == nullptr);
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
        const std::size_t contextSelectionCount =
            selectedClipCount(snapshot);
        const bool groupSelection = contextSelectionCount > 1;
        const bool canSplit = selectedClipsCanSplitAtFrame(
            snapshot, snapshot.transport.currentFrame);
        if (ImGui::MenuItem(
                groupSelection
                    ? "Split Selected At Playhead"
                    : "Split At Playhead",
                "Ctrl+B",
                false,
                canSplit)) {
            if (groupSelection) {
                applyCommand(
                    shellState,
                    jcut::SplitSelectedClipsCommand{
                        snapshot.transport.currentFrame});
            } else if (contextClip) {
                applyCommand(shellState, jcut::SplitClipCommand{
                    contextClip->id,
                    snapshot.transport.currentFrame});
            }
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
        const bool canLockTransformToSource =
            contextClip &&
            !contextClip->locked &&
            !contextClip->linkedSourceClipId.empty() &&
            contextClip->mediaKind != "audio";
        if (ImGui::MenuItem(
                "Lock Transform To Source",
                nullptr,
                contextClip && contextClip->sourceTransformLocked,
                canLockTransformToSource) &&
            contextClip) {
            applyCommand(
                shellState,
                jcut::SetClipSourceTransformLockedCommand{
                    contextClip->id,
                    !contextClip->sourceTransformLocked});
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
        if (contextClip) {
            ImGui::Separator();
            if (ImGui::MenuItem("Copy Clip Name")) {
                ImGui::SetClipboardText(contextClip->label.c_str());
                shellState->statusMessage = "clip name copied";
            }
            const bool isTitle = contextClip->mediaKind == "title";
            if (ImGui::MenuItem(
                    "Copy title", nullptr, false, isTitle)) {
                const std::int64_t localFrame = std::clamp<std::int64_t>(
                    shellState->timelineContextFrame -
                        contextClip->startFrame,
                    0,
                    std::max(0, contextClip->durationFrames - 1));
                const jcut::EditorTitleKeyframe title =
                    jcut::evaluateEditorClipTitleAtLocalFrame(
                        *contextClip, localFrame);
                ImGui::SetClipboardText(title.text.c_str());
                shellState->statusMessage = "title text copied";
            }
            if (ImGui::MenuItem("Grading...")) {
                shellState->requestedInspectorTab = "Grade";
            }
            if (ImGui::MenuItem("Refresh")) {
                refreshClipMetadata(
                    shellState, snapshot, contextClip->id);
            }
            if (ImGui::MenuItem("Sync...")) {
                shellState->requestedInspectorTab = "Sync";
            }
            if (ImGui::BeginMenu(
                    "Generated Clips",
                    jcut::editorClipHasVisualsCore(*contextClip))) {
                if (ImGui::MenuItem("Add Mask Matte Layer")) {
                    shellState->maskSidecarContextClipId =
                        contextClip->id;
                    shellState->requestedInspectorTab = "Masks";
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu(
                    "Rotoscope",
                    jcut::editorClipHasVisualsCore(*contextClip) &&
                        !isTitle)) {
                if (ImGui::MenuItem("Run SAM 3...")) {
                    shellState->promptMaskSourceClipId =
                        contextClip->id;
                    shellState->requestedInspectorTab = "Masks";
                }
                if (ImGui::MenuItem("Run BiRefNet...")) {
                    shellState->birefnetSourceClipId =
                        contextClip->id;
                    shellState->requestedInspectorTab = "Masks";
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Transcript")) {
                if (ImGui::MenuItem(
                        "Transcribe",
                        nullptr,
                        false,
                        contextClip->hasAudio)) {
                    startTranscriptionJob(
                        shellState, *contextClip);
                }
                if (ImGui::MenuItem("Open Transcript Tools")) {
                    shellState->requestedInspectorTab = "Transcript";
                }
                if (ImGui::MenuItem("Apply Speaker Title Fly-In")) {
                    shellState->requestedInspectorTab = "Speakers";
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu(
                    "Proxy",
                    jcut::editorClipHasVisualsCore(*contextClip) &&
                        !isTitle)) {
                if (ImGui::MenuItem("Open Proxy Controls")) {
                    shellState->requestedInspectorTab = "Clip";
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu(
                    "FaceDetections",
                    jcut::editorClipHasVisualsCore(*contextClip) &&
                        !isTitle)) {
                if (ImGui::MenuItem("Generate / Inspect...")) {
                    shellState->requestedInspectorTab = "Speakers";
                }
                if (ImGui::MenuItem("Open Job Status")) {
                    shellState->requestedInspectorTab = "Jobs";
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Properties")) {
                shellState->requestedInspectorTab = "Properties";
            }
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
            const bool anySelectedUnlocked = std::any_of(
                snapshot.clips.begin(),
                snapshot.clips.end(),
                [](const jcut::EditorClip& clip) {
                    return clip.selected &&
                        jcut::canonicalEditorClipRole(clip.clipRole) !=
                            "mask_matte" &&
                        !clip.locked;
                });
            if (groupSelection &&
                ImGui::MenuItem(
                    anySelectedUnlocked
                        ? "Lock Selected"
                        : "Unlock Selected")) {
                applyCommand(
                    shellState,
                    jcut::SetSelectedClipsLockedCommand{
                        anySelectedUnlocked});
            } else if (!groupSelection &&
                       ImGui::MenuItem(
                           contextClip->locked ? "Unlock" : "Lock")) {
                applyCommand(shellState, jcut::SetClipLockedCommand{
                    contextClip->id, !contextClip->locked});
            }
        }
        ImGui::EndDisabled();
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

void drawInspectorPanel(ShellState* shellState, const jcut::EditorDocumentCore& snapshot)
{
    pollAiOperations(shellState);
    const std::string requestedInspectorTab =
        std::exchange(shellState->requestedInspectorTab, {});
    const auto inspectorTabFlags = [&requestedInspectorTab](
                                       const char* label) {
        return requestedInspectorTab == label
            ? ImGuiTabItemFlags_SetSelected
            : ImGuiTabItemFlags_None;
    };
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
        if (ImGui::BeginTabItem(
                "Grade", nullptr, inspectorTabFlags("Grade"))) {
            drawInspectorHeading("Grade", snapshot, currentClip);
            float saturation = currentClip ? static_cast<float>(currentClip->saturation) : 1.0f;
            float brightness = currentClip ? static_cast<float>(currentClip->brightness) : 0.0f;
            float contrast = currentClip ? static_cast<float>(currentClip->contrast) : 1.0f;
            bool gradePreview = currentClip ? currentClip->gradingPreviewEnabled : false;
            ImGui::BeginDisabled(!currentClip);
            bool gradingChanged = false;
            gradingChanged |= ImGui::SliderFloat("Saturation", &saturation, -10.0f, 10.0f, "%.2f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            gradingChanged |= ImGui::SliderFloat("Brightness", &brightness, -10.0f, 10.0f, "%.2f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            gradingChanged |= ImGui::SliderFloat("Contrast", &contrast, -10.0f, 10.0f, "%.2f");
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
                            const auto applyLockedTone = [](
                                const std::vector<jcut::EditorPoint>& curve,
                                double* shadows,
                                double* midtones,
                                double* highlights) {
                                const jcut::EditorToneValues tones =
                                    jcut::editorToneValuesFromThreePointCurve(curve);
                                *shadows = tones.shadows;
                                *midtones = tones.midtones;
                                *highlights = tones.highlights;
                            };
                            const bool redCurveChanged =
                                drawGradingCurvePointEditor(
                                    "Red",
                                    &draft->curvePointsR,
                                    draft->curveThreePointLock,
                                    draft->curveSmoothingEnabled);
                            const bool greenCurveChanged =
                                drawGradingCurvePointEditor(
                                    "Green",
                                    &draft->curvePointsG,
                                    draft->curveThreePointLock,
                                    draft->curveSmoothingEnabled);
                            const bool blueCurveChanged =
                                drawGradingCurvePointEditor(
                                    "Blue",
                                    &draft->curvePointsB,
                                    draft->curveThreePointLock,
                                    draft->curveSmoothingEnabled);
                            if (draft->curveThreePointLock) {
                                if (redCurveChanged) {
                                    applyLockedTone(
                                        draft->curvePointsR,
                                        &draft->shadowsR,
                                        &draft->midtonesR,
                                        &draft->highlightsR);
                                }
                                if (greenCurveChanged) {
                                    applyLockedTone(
                                        draft->curvePointsG,
                                        &draft->shadowsG,
                                        &draft->midtonesG,
                                        &draft->highlightsG);
                                }
                                if (blueCurveChanged) {
                                    applyLockedTone(
                                        draft->curvePointsB,
                                        &draft->shadowsB,
                                        &draft->midtonesB,
                                        &draft->highlightsB);
                                }
                            }
                            drawGradingCurvePointEditor(
                                "Luma",
                                &draft->curvePointsLuma,
                                false,
                                draft->curveSmoothingEnabled);
                            ImGui::TreePop();
                        }
                    });
            if (ImGui::TreeNode("Auto Oppose Settings")) {
                jcut::EditorAutoOpposeSettingsCore& settings =
                    shellState->autoOpposeSettings;
                ImGui::SliderInt(
                    "Analysis Density", &settings.sampleTarget, 30, 2000);
                ImGui::SliderInt(
                    "Min Event Gap", &settings.minEventGapFrames, 1, 300);
                ImGui::SliderInt(
                    "Max Events", &settings.maxEvents, 1, 200);
                constexpr double kJumpThresholdMinimum = 0.01;
                constexpr double kJumpThresholdMaximum = 0.5;
                ImGui::SliderScalar(
                    "Luma Jump Threshold",
                    ImGuiDataType_Double,
                    &settings.jumpLumaThreshold,
                    &kJumpThresholdMinimum,
                    &kJumpThresholdMaximum,
                    "%.3f");
                settings.jumpLumaThreshold =
                    std::clamp(settings.jumpLumaThreshold, 0.01, 0.5);
                ImGui::SliderScalar(
                    "Saturation Jump Threshold",
                    ImGuiDataType_Double,
                    &settings.jumpSaturationThreshold,
                    &kJumpThresholdMinimum,
                    &kJumpThresholdMaximum,
                    "%.3f");
                settings.jumpSaturationThreshold =
                    std::clamp(settings.jumpSaturationThreshold, 0.01, 0.5);
                ImGui::SliderScalar(
                    "Contrast Jump Threshold",
                    ImGuiDataType_Double,
                    &settings.jumpContrastThreshold,
                    &kJumpThresholdMinimum,
                    &kJumpThresholdMaximum,
                    "%.3f");
                settings.jumpContrastThreshold =
                    std::clamp(settings.jumpContrastThreshold, 0.01, 0.5);
                constexpr double kOpposeStrengthMinimum = 0.5;
                constexpr double kOpposeStrengthMaximum = 6.0;
                ImGui::SliderScalar(
                    "Brightness Oppose Strength",
                    ImGuiDataType_Double,
                    &settings.brightnessStrength,
                    &kOpposeStrengthMinimum,
                    &kOpposeStrengthMaximum,
                    "%.2f");
                ImGui::TreePop();
            }
            ImGui::BeginDisabled(
                !currentClip || shellState->autoOpposeRunning);
            if (ImGui::Button(
                    shellState->autoOpposeRunning
                        ? "Auto Oppose: Analyzing..."
                        : "Auto Oppose")) {
                startAutoOpposeJob(shellState, snapshot, *currentClip);
            }
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
                        markInspectorDeleteTargetForLastItem(
                            shellState,
                            InspectorDeleteTargetKind::ClipKeyframe,
                            currentClip->id,
                            jcut::EditorKeyframeChannel::Grading,
                            keyframe.frame);
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
                        markInspectorDeleteTargetForLastItem(
                            shellState,
                            InspectorDeleteTargetKind::ClipKeyframe,
                            currentClip->id,
                            jcut::EditorKeyframeChannel::Opacity,
                            keyframe.frame);
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
            std::string edgeFillEffect =
                currentClip ? currentClip->edgeFillEffect : "none";
            int edgeFillPixels = currentClip ? currentClip->edgeFillPixels : 1;
            float edgeFillPower = currentClip
                ? static_cast<float>(currentClip->edgeFillPower) : 2.0f;
            float edgeFillOpacity = currentClip
                ? static_cast<float>(currentClip->edgeFillOpacity) : 1.0f;
            float edgeFillBrightness = currentClip
                ? static_cast<float>(currentClip->edgeFillBrightness) : 0.0f;
            float edgeFillSaturation = currentClip
                ? static_cast<float>(currentClip->edgeFillSaturation) : 1.0f;
            if (ImGui::CollapsingHeader(
                    "Edge Fill", ImGuiTreeNodeFlags_DefaultOpen)) {
                static constexpr std::array<std::pair<std::string_view, std::string_view>, 7>
                    edgeFillOptions{{
                        {"none", "None"},
                        {"edge_stretch", "Edge Stretch"},
                        {"progressive_edge_stretch", "Progressive Edge Stretch"},
                        {"progressive_bidirectional_edge_stretch",
                         "Progressive Bidirectional Edge Stretch"},
                        {"tile", "Tile"},
                        {"mirror", "Mirror"},
                        {"blur_cover", "Blur Cover"},
                    }};
                std::string_view edgeFillLabel = "None";
                for (const auto& [id, label] : edgeFillOptions) {
                    if (edgeFillEffect == id) {
                        edgeFillLabel = label;
                        break;
                    }
                }
                if (ImGui::BeginCombo("Effect", edgeFillLabel.data())) {
                    for (const auto& [id, label] : edgeFillOptions) {
                        const bool selected = edgeFillEffect == id;
                        if (ImGui::Selectable(label.data(), selected)) {
                            edgeFillEffect = id;
                            effectChanged = true;
                        }
                        if (selected) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }
                const bool edgeFillEnabled = edgeFillEffect != "none";
                const bool progressiveEdgeFill =
                    edgeFillEffect == "progressive_edge_stretch" ||
                    edgeFillEffect == "progressive_bidirectional_edge_stretch";
                ImGui::BeginDisabled(!edgeFillEnabled);
                ImGui::BeginDisabled(
                    !progressiveEdgeFill);
                effectChanged |= ImGui::SliderInt(
                    "Edge Width", &edgeFillPixels, 1, 512);
                effectChanged |= ImGui::SliderFloat(
                    "Curve Power", &edgeFillPower, 0.25f, 8.0f, "%.2f");
                ImGui::EndDisabled();
                effectChanged |= ImGui::SliderFloat(
                    "Edge Opacity", &edgeFillOpacity, 0.0f, 1.0f, "%.2f");
                effectChanged |= ImGui::SliderFloat(
                    "Edge Brightness", &edgeFillBrightness, -1.0f, 1.0f, "%.2f");
                effectChanged |= ImGui::SliderFloat(
                    "Edge Saturation", &edgeFillSaturation, 0.0f, 3.0f, "%.2f");
                ImGui::EndDisabled();
            }
            if (ImGui::BeginCombo("Preset", presetLabel.c_str())) {
                for (const std::string_view optionId : jcut::kEditorEffectPresetIds) {
                    if (optionId == "progressive_edge_stretch") {
                        continue;
                    }
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
                command.edgeFillEffect = edgeFillEffect;
                command.edgeFillPixels = edgeFillPixels;
                command.edgeFillPower = edgeFillPower;
                command.edgeFillOpacity = edgeFillOpacity;
                command.edgeFillBrightness = edgeFillBrightness;
                command.edgeFillSaturation = edgeFillSaturation;
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
        if (ImGui::BeginTabItem(
                "Masks", nullptr, inspectorTabFlags("Masks"))) {
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
            ImGui::SeparatorText("BiRefNet Alpha Generator");
            const jcut::jobs::BiRefNetJobSnapshotCore birefnetJob =
                shellState->birefnetJob.snapshot();
            if (birefnetJob.state !=
                    jcut::jobs::ProcessJobSnapshotCore::State::Idle) {
                ImGui::TextWrapped("%s", birefnetJob.status.c_str());
                if (birefnetJob.totalFrames > 0) {
                    ImGui::ProgressBar(
                        static_cast<float>(
                            std::clamp(
                                birefnetJob.percent / 100.0,
                                0.0,
                                1.0)),
                        ImVec2(-1.0f, 0.0f));
                    ImGui::TextDisabled(
                        "%lld / %lld frames",
                        static_cast<long long>(
                            birefnetJob.currentFrame),
                        static_cast<long long>(
                            birefnetJob.totalFrames));
                }
                if (!birefnetJob.livePreviewPath.empty()) {
                    ImGui::TextDisabled(
                        "Live preview: %s",
                        birefnetJob.livePreviewPath.c_str());
                }
                if (shellState->birefnetLivePreviewTextureId != 0 &&
                    shellState->birefnetLivePreviewSize.valid()) {
                    const float availableWidth = std::max(
                        160.0f,
                        ImGui::GetContentRegionAvail().x);
                    const float width = std::min(
                        availableWidth, 960.0f);
                    const float height = width *
                        static_cast<float>(
                            shellState->
                                birefnetLivePreviewSize.height) /
                        static_cast<float>(
                            shellState->
                                birefnetLivePreviewSize.width);
                    ImGui::Image(
                        shellState->
                            birefnetLivePreviewTextureId,
                        ImVec2(width, height));
                } else if (!shellState->
                               birefnetLivePreviewError.empty()) {
                    ImGui::TextDisabled(
                        "%s",
                        shellState->
                            birefnetLivePreviewError.c_str());
                } else if (birefnetJob.active()) {
                    ImGui::TextDisabled(
                        "Waiting for the first BiRefNet live preview...");
                }
            }
            if (maskSourceClip) {
                ImGui::BeginDisabled(birefnetJob.active());
                inputTextForString<512>(
                    "BiRefNet Model",
                    &shellState->birefnetModel);
                inputTextForString<512>(
                    "BiRefNet Revision",
                    &shellState->birefnetRevision);
                inputTextForString<512>(
                    "BiRefNet Model Cache",
                    &shellState->birefnetModelCachePath);
                inputTextForString<512>(
                    "BiRefNet Runtime Cache",
                    &shellState->birefnetRuntimeCachePath);
                const char* devices[] = {"CUDA", "CPU"};
                ImGui::SetNextItemWidth(140.0f);
                ImGui::Combo(
                    "BiRefNet Device",
                    &shellState->birefnetDevice,
                    devices,
                    IM_ARRAYSIZE(devices));
                ImGui::BeginDisabled(
                    shellState->birefnetDevice == 1);
                ImGui::Checkbox(
                    "BiRefNet FP16",
                    &shellState->birefnetFp16);
                ImGui::EndDisabled();
                if (shellState->birefnetDevice == 1) {
                    shellState->birefnetFp16 = false;
                }
                ImGui::SetNextItemWidth(140.0f);
                ImGui::SliderFloat(
                    "Alpha Tolerance (%)",
                    &shellState->birefnetAlphaTolerancePercent,
                    0.0f,
                    99.0f,
                    "%.1f");
                ImGui::Checkbox(
                    "Run BiRefNet Docker container as root",
                    &shellState->birefnetDockerRoot);
                ImGui::Checkbox(
                    "Restart and replace prior BiRefNet output",
                    &shellState->birefnetRestart);
                if (ImGui::Button("Run BiRefNet Job")) {
                    startBiRefNetJob(
                        shellState, *maskSourceClip);
                    saveUiPreferences(*shellState);
                }
                ImGui::EndDisabled();
                if (birefnetJob.active()) {
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel BiRefNet Job")) {
                        shellState->birefnetJob.cancel();
                    }
                }
            } else {
                ImGui::TextWrapped(
                    "Select a video source or generated Mask Matte child to run BiRefNet.");
            }
            ImGui::SeparatorText("Prompt Mask Generator");
            const jcut::masks::PromptMaskJobSnapshot promptJob =
                shellState->promptMaskJob.snapshot();
            const int promptJobState =
                static_cast<int>(promptJob.state);
            if (shellState->promptMaskLastState != promptJobState) {
                shellState->promptMaskLastState = promptJobState;
                if (promptJob.state ==
                        jcut::masks::PromptMaskJobSnapshot::State::Completed) {
                    if (shellState->promptMaskSourceClipId > 0 &&
                        !promptJob.selectedMaskPath.empty()) {
                        applyCommand(
                            shellState,
                            jcut::MaterializeMaskMatteCommand{
                                shellState->promptMaskSourceClipId,
                                promptJob.selectedMaskPath,
                                promptJob.selectedMaskId,
                                promptJob.selectedMaskName});
                    }
                    shellState->statusMessage = promptJob.status;
                    shellState->maskSidecarContextClipId = -1;
                } else if (
                    promptJob.state ==
                        jcut::masks::PromptMaskJobSnapshot::State::Failed ||
                    promptJob.state ==
                        jcut::masks::PromptMaskJobSnapshot::State::Paused) {
                    shellState->statusMessage = promptJob.status;
                }
            }
            if (promptJob.state !=
                    jcut::masks::PromptMaskJobSnapshot::State::Idle) {
                ImGui::TextWrapped("%s", promptJob.status.c_str());
                if (!promptJob.selectedMaskPath.empty()) {
                    ImGui::TextDisabled(
                        "Result: %s",
                        promptJob.selectedMaskPath.c_str());
                }
                if (!promptJob.logPath.empty()) {
                    ImGui::TextDisabled(
                        "Log: %s", promptJob.logPath.c_str());
                }
            }
            if (maskSourceClip) {
                ImGui::BeginDisabled(promptJob.active());
                inputTextForString<512>(
                    "Text Prompt", &shellState->promptMaskPrompt);
                inputTextForString<512>(
                    "SAM3 Model Cache",
                    &shellState->promptMaskModelCachePath);
                inputTextForString<512>(
                    "SAM3 Runtime Cache",
                    &shellState->promptMaskRuntimeCachePath);
                ImGui::SetNextItemWidth(140.0f);
                ImGui::InputInt(
                    "Scale Width",
                    &shellState->promptMaskScaleWidth,
                    64,
                    256);
                shellState->promptMaskScaleWidth = std::clamp(
                    shellState->promptMaskScaleWidth, 0, 8192);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(140.0f);
                ImGui::InputInt(
                    "Prescale Width",
                    &shellState->promptMaskPrescaleWidth,
                    64,
                    256);
                shellState->promptMaskPrescaleWidth = std::clamp(
                    shellState->promptMaskPrescaleWidth, 0, 8192);
                ImGui::BeginDisabled(
                    shellState->promptMaskVideoMode ||
                    shellState->promptMaskWriteBinaryMasks);
                ImGui::SetNextItemWidth(140.0f);
                ImGui::InputFloat(
                    "Extract FPS",
                    &shellState->promptMaskExtractFps,
                    1.0f,
                    5.0f,
                    "%.3f");
                shellState->promptMaskExtractFps = std::clamp(
                    shellState->promptMaskExtractFps, 0.0f, 240.0f);
                ImGui::EndDisabled();
                const char* frameFormats[] = {"JPEG", "PNG"};
                ImGui::BeginDisabled(
                    shellState->promptMaskVideoMode);
                ImGui::SetNextItemWidth(140.0f);
                ImGui::Combo(
                    "Intermediate Frames",
                    &shellState->promptMaskFrameFormat,
                    frameFormats,
                    IM_ARRAYSIZE(frameFormats));
                ImGui::EndDisabled();
                ImGui::Checkbox(
                    "Enable torch.compile",
                    &shellState->promptMaskCompileModel);
                const bool videoModeChanged = ImGui::Checkbox(
                    "Run SAM video mode",
                    &shellState->promptMaskVideoMode);
                if (videoModeChanged) {
                    shellState->promptMaskWriteBinaryMasks =
                        !shellState->promptMaskVideoMode;
                    shellState->promptMaskUnionCurrent = false;
                    shellState->promptMaskWritePreviewFrames = false;
                }
                ImGui::BeginDisabled(shellState->promptMaskVideoMode);
                ImGui::Checkbox(
                    "Write binary mask frames",
                    &shellState->promptMaskWriteBinaryMasks);
                const bool canUnion =
                    maskEditClip && !maskEditClip->maskFramesDir.empty();
                ImGui::BeginDisabled(
                    !shellState->promptMaskWriteBinaryMasks ||
                    !canUnion);
                ImGui::Checkbox(
                    "Union with selected matte",
                    &shellState->promptMaskUnionCurrent);
                ImGui::EndDisabled();
                if (!canUnion) {
                    shellState->promptMaskUnionCurrent = false;
                }
                ImGui::Checkbox(
                    "Write masked preview frames",
                    &shellState->promptMaskWritePreviewFrames);
                ImGui::EndDisabled();
                ImGui::Checkbox(
                    "Export centers JSONL",
                    &shellState->promptMaskExportCenters);
                ImGui::Checkbox(
                    "Run Docker container as root",
                    &shellState->promptMaskDockerRoot);
                ImGui::Checkbox(
                    "Restart and replace prior prompt outputs",
                    &shellState->promptMaskRestart);
                if (ImGui::Button("Run Prompt Mask Job")) {
                    const std::optional<fs::path> script =
                        sam3ScriptPath();
                    jcut::masks::PromptMaskJobRequest request;
                    request.scriptPath =
                        script ? script->string() : std::string{};
                    request.mediaPath =
                        resolvedClipMediaPathForProbe(
                            *shellState, *maskSourceClip).string();
                    request.prompt = shellState->promptMaskPrompt;
                    request.modelCachePath =
                        shellState->promptMaskModelCachePath;
                    request.runtimeCachePath =
                        shellState->promptMaskRuntimeCachePath;
                    request.currentMaskDirectory =
                        canUnion ? maskEditClip->maskFramesDir
                                 : std::string{};
                    if (!request.currentMaskDirectory.empty()) {
                        fs::path current(
                            request.currentMaskDirectory);
                        if (current.is_relative() &&
                            !shellState->projectRootPath.empty()) {
                            current =
                                fs::path(shellState->projectRootPath) /
                                current;
                        }
                        request.currentMaskDirectory =
                            current.lexically_normal().string();
                    }
                    request.scaleWidth =
                        shellState->promptMaskScaleWidth;
                    request.prescaleWidth =
                        shellState->promptMaskPrescaleWidth;
                    request.extractFps =
                        shellState->promptMaskExtractFps;
                    request.intermediateFramesFormat =
                        shellState->promptMaskFrameFormat == 1
                            ? "png"
                            : "jpg";
                    request.compileModel =
                        shellState->promptMaskCompileModel;
                    request.videoMode =
                        shellState->promptMaskVideoMode;
                    request.writeBinaryMasks =
                        shellState->promptMaskWriteBinaryMasks;
                    request.unionWithCurrentMask =
                        shellState->promptMaskUnionCurrent;
                    request.writeMaskPreviewFrames =
                        shellState->promptMaskWritePreviewFrames;
                    request.exportCentersJson =
                        shellState->promptMaskExportCenters;
                    request.runDockerAsRoot =
                        shellState->promptMaskDockerRoot;
                    request.restartPolicy =
                        shellState->promptMaskRestart
                            ? jcut::masks::
                                  PromptMaskRestartPolicy::Restart
                            : jcut::masks::
                                  PromptMaskRestartPolicy::Resume;
                    std::string error;
                    shellState->promptMaskSourceClipId =
                        maskSourceClip->id;
                    if (!shellState->promptMaskJob.start(
                            request, &error)) {
                        shellState->statusMessage =
                            error.empty()
                                ? "prompt-mask job could not start"
                                : error;
                    } else {
                        shellState->promptMaskLastState = -1;
                        shellState->statusMessage =
                            "SAM3 prompt-mask job started";
                        saveUiPreferences(*shellState);
                    }
                }
                ImGui::EndDisabled();
                if (promptJob.active()) {
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel Prompt Mask Job")) {
                        shellState->promptMaskJob.cancel();
                    }
                }
            } else {
                ImGui::TextWrapped(
                    "Select a video source or generated Mask Matte child to run SAM3.");
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
                "Mask Red Curve", &gradeCurveR, false, gradeCurveSmoothing);
            maskCurveChanged |= drawGradingCurvePointEditor(
                "Mask Green Curve", &gradeCurveG, false, gradeCurveSmoothing);
            maskCurveChanged |= drawGradingCurvePointEditor(
                "Mask Blue Curve", &gradeCurveB, false, gradeCurveSmoothing);
            maskCurveChanged |= drawGradingCurvePointEditor(
                "Mask Luma Curve", &gradeCurveLuma, false, gradeCurveSmoothing);
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
                        markInspectorDeleteTargetForLastItem(
                            shellState,
                            InspectorDeleteTargetKind::TitleKeyframe,
                            currentClip->id,
                            jcut::EditorKeyframeChannel::Transform,
                            keyframe.frame);
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
        if (ImGui::BeginTabItem(
                "Sync", nullptr, inspectorTabFlags("Sync"))) {
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
                    const std::string syncOwnerLabel =
                        marker.clipId + "##sync-owner";
                    if (ImGui::Selectable(syncOwnerLabel.c_str())) {
                        applyCommand(
                            shellState,
                            jcut::SeekToFrameCommand{
                                static_cast<int>(
                                    std::clamp<std::int64_t>(
                                        marker.frame,
                                        0,
                                        std::numeric_limits<int>::max()))});
                    }
                    markSyncDeleteTargetForLastItem(shellState, marker);
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
            if (ImGui::CollapsingHeader(
                    "Automatic Speaker Framing",
                    ImGuiTreeNodeFlags_DefaultOpen)) {
                bool framingEnabled =
                    currentClip && currentClip->speakerFramingEnabled;
                float bakedTargetX = currentClip
                    ? static_cast<float>(
                          currentClip->speakerFramingBakedTargetXNorm)
                    : 0.5f;
                float bakedTargetY = currentClip
                    ? static_cast<float>(
                          currentClip->speakerFramingBakedTargetYNorm)
                    : 0.35f;
                float bakedTargetBox = currentClip
                    ? static_cast<float>(
                          currentClip->speakerFramingBakedTargetBoxNorm)
                    : -1.0f;
                float minimumConfidence = currentClip
                    ? static_cast<float>(
                          currentClip->speakerFramingMinConfidence)
                    : 0.08f;
                int manualTrackId = currentClip
                    ? currentClip->speakerFramingManualTrackId : -1;
                std::string manualStreamId = currentClip
                    ? currentClip->speakerFramingManualStreamId
                    : std::string{};
                int centerSmoothingFrames = currentClip
                    ? currentClip->speakerFramingCenterSmoothingFrames : 0;
                int zoomSmoothingFrames = currentClip
                    ? currentClip->speakerFramingZoomSmoothingFrames : 0;
                int smoothingMode = currentClip
                    ? currentClip->speakerFramingSmoothingMode : 0;
                float centerSmoothingStrength = currentClip
                    ? static_cast<float>(
                          currentClip
                              ->speakerFramingCenterSmoothingStrength)
                    : 1.0f;
                float zoomSmoothingStrength = currentClip
                    ? static_cast<float>(
                          currentClip
                              ->speakerFramingZoomSmoothingStrength)
                    : 1.0f;
                int gapHoldFrames = currentClip
                    ? currentClip->speakerFramingGapHoldFrames : 0;
                bool framingSettingsChanged = false;
                framingSettingsChanged |= ImGui::Checkbox(
                    "Enabled##speakerFraming", &framingEnabled);
                framingSettingsChanged |= ImGui::SliderFloat(
                    "Baked Target X",
                    &bakedTargetX,
                    0.0f,
                    1.0f,
                    "%.3f");
                beginRuntimeHistoryTransactionForLastItem(shellState);
                framingSettingsChanged |= ImGui::SliderFloat(
                    "Baked Target Y",
                    &bakedTargetY,
                    0.0f,
                    1.0f,
                    "%.3f");
                beginRuntimeHistoryTransactionForLastItem(shellState);
                framingSettingsChanged |= ImGui::SliderFloat(
                    "Baked Target Box",
                    &bakedTargetBox,
                    -1.0f,
                    1.0f,
                    "%.3f");
                beginRuntimeHistoryTransactionForLastItem(shellState);
                framingSettingsChanged |= ImGui::SliderFloat(
                    "Minimum Confidence",
                    &minimumConfidence,
                    0.0f,
                    1.0f,
                    "%.3f");
                beginRuntimeHistoryTransactionForLastItem(shellState);
                framingSettingsChanged |= ImGui::InputInt(
                    "Manual Track ID", &manualTrackId);
                framingSettingsChanged |= inputTextForString<128>(
                    "Manual Stream ID", &manualStreamId);
                framingSettingsChanged |= ImGui::SliderInt(
                    "Center Smoothing Frames",
                    &centerSmoothingFrames,
                    0,
                    500);
                beginRuntimeHistoryTransactionForLastItem(shellState);
                framingSettingsChanged |= ImGui::SliderInt(
                    "Zoom Smoothing Frames",
                    &zoomSmoothingFrames,
                    0,
                    500);
                beginRuntimeHistoryTransactionForLastItem(shellState);
                constexpr const char* smoothingModes[] = {
                    "Centered",
                    "Causal",
                    "Forward"};
                framingSettingsChanged |= ImGui::Combo(
                    "Smoothing Mode",
                    &smoothingMode,
                    smoothingModes,
                    IM_ARRAYSIZE(smoothingModes));
                framingSettingsChanged |= ImGui::SliderFloat(
                    "Center Smoothing Strength",
                    &centerSmoothingStrength,
                    0.0f,
                    5.0f,
                    "%.2f");
                beginRuntimeHistoryTransactionForLastItem(shellState);
                framingSettingsChanged |= ImGui::SliderFloat(
                    "Zoom Smoothing Strength",
                    &zoomSmoothingStrength,
                    0.0f,
                    5.0f,
                    "%.2f");
                beginRuntimeHistoryTransactionForLastItem(shellState);
                framingSettingsChanged |= ImGui::SliderInt(
                    "Gap Hold Frames", &gapHoldFrames, 0, 240);
                beginRuntimeHistoryTransactionForLastItem(shellState);
                if (framingSettingsChanged && currentClip) {
                    applyCommand(
                        shellState,
                        jcut::SetClipSpeakerFramingCommand{
                            currentClip->id,
                            framingEnabled,
                            bakedTargetX,
                            bakedTargetY,
                            bakedTargetBox,
                            minimumConfidence,
                            manualTrackId,
                            manualStreamId,
                            centerSmoothingFrames,
                            zoomSmoothingFrames,
                            smoothingMode,
                            centerSmoothingStrength,
                            zoomSmoothingStrength,
                            gapHoldFrames});
                }

                if (ImGui::Button("Enable At Playhead") &&
                    currentClip) {
                    applyCommand(
                        shellState,
                        jcut::UpsertSpeakerFramingEnabledKeyframeCommand{
                            currentClip->id,
                            {currentClipLocalFrame, true}});
                }
                ImGui::SameLine();
                if (ImGui::Button("Disable At Playhead") &&
                    currentClip) {
                    applyCommand(
                        shellState,
                        jcut::UpsertSpeakerFramingEnabledKeyframeCommand{
                            currentClip->id,
                            {currentClipLocalFrame, false}});
                }

                const auto latestTransformAtPlayhead =
                    [&](const std::vector<jcut::EditorTransformKeyframe>&
                            keyframes,
                        jcut::EditorTransformKeyframe fallback) {
                        for (const auto& keyframe : keyframes) {
                            if (keyframe.frame >
                                currentClipLocalFrame) {
                                break;
                            }
                            fallback = keyframe;
                        }
                        fallback.frame = currentClipLocalFrame;
                        return fallback;
                    };
                jcut::EditorTransformKeyframe framingDraft{
                    currentClipLocalFrame,
                    "Baked Framing",
                    0.0,
                    0.0,
                    0.0,
                    1.0,
                    1.0,
                    true};
                jcut::EditorTransformKeyframe targetDraft{
                    currentClipLocalFrame,
                    "Framing Target",
                    0.5,
                    0.35,
                    0.0,
                    -1.0,
                    -1.0,
                    true};
                if (currentClip) {
                    framingDraft = latestTransformAtPlayhead(
                        currentClip->speakerFramingKeyframes,
                        framingDraft);
                    targetDraft = latestTransformAtPlayhead(
                        currentClip->speakerFramingTargetKeyframes,
                        targetDraft);
                }
                ImGui::SeparatorText("Baked Transform At Playhead");
                ImGui::InputDouble(
                    "Baked Translate X",
                    &framingDraft.translationX,
                    1.0,
                    10.0,
                    "%.3f");
                ImGui::InputDouble(
                    "Baked Translate Y",
                    &framingDraft.translationY,
                    1.0,
                    10.0,
                    "%.3f");
                ImGui::InputDouble(
                    "Baked Rotation",
                    &framingDraft.rotation,
                    0.1,
                    1.0,
                    "%.3f");
                ImGui::InputDouble(
                    "Baked Scale X",
                    &framingDraft.scaleX,
                    0.01,
                    0.1,
                    "%.3f");
                ImGui::InputDouble(
                    "Baked Scale Y",
                    &framingDraft.scaleY,
                    0.01,
                    0.1,
                    "%.3f");
                if (ImGui::Button("Set Baked Transform") &&
                    currentClip) {
                    applyCommand(
                        shellState,
                        jcut::UpsertSpeakerFramingKeyframeCommand{
                            currentClip->id, framingDraft});
                }
                ImGui::SeparatorText("Dynamic Target At Playhead");
                ImGui::InputDouble(
                    "Target X",
                    &targetDraft.translationX,
                    0.01,
                    0.1,
                    "%.3f");
                ImGui::InputDouble(
                    "Target Y",
                    &targetDraft.translationY,
                    0.01,
                    0.1,
                    "%.3f");
                ImGui::InputDouble(
                    "Target Box",
                    &targetDraft.scaleX,
                    0.01,
                    0.1,
                    "%.3f");
                targetDraft.scaleY = targetDraft.scaleX;
                if (ImGui::Button("Set Dynamic Target") &&
                    currentClip) {
                    applyCommand(
                        shellState,
                        jcut::UpsertSpeakerFramingTargetKeyframeCommand{
                            currentClip->id, targetDraft});
                }

                const auto drawFramingRows =
                    [&](const char* tableId,
                        const char* label,
                        const std::vector<jcut::EditorTransformKeyframe>&
                            keyframes,
                        jcut::EditorKeyframeChannel channel) {
                        ImGui::SeparatorText(label);
                        if (!ImGui::BeginTable(
                                tableId,
                                4,
                                ImGuiTableFlags_Borders |
                                    ImGuiTableFlags_RowBg)) {
                            return;
                        }
                        ImGui::TableSetupColumn("Frame");
                        ImGui::TableSetupColumn("Position");
                        ImGui::TableSetupColumn("Scale");
                        ImGui::TableSetupColumn("Actions");
                        ImGui::TableHeadersRow();
                        for (const auto& keyframe : keyframes) {
                            ImGui::PushID(
                                static_cast<int>(keyframe.frame));
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::Text(
                                "%lld",
                                static_cast<long long>(
                                    keyframe.frame));
                            ImGui::TableNextColumn();
                            ImGui::Text(
                                "%.3f, %.3f",
                                keyframe.translationX,
                                keyframe.translationY);
                            ImGui::TableNextColumn();
                            ImGui::Text(
                                "%.3f, %.3f",
                                keyframe.scaleX,
                                keyframe.scaleY);
                            ImGui::TableNextColumn();
                            if (ImGui::SmallButton("Seek")) {
                                applyCommand(
                                    shellState,
                                    jcut::SeekToFrameCommand{
                                        currentClip->startFrame +
                                        static_cast<int>(
                                            keyframe.frame)});
                            }
                            ImGui::SameLine();
                            if (ImGui::SmallButton("Remove")) {
                                applyCommand(
                                    shellState,
                                    jcut::RemoveClipKeyframeCommand{
                                        currentClip->id,
                                        channel,
                                        keyframe.frame});
                            }
                            ImGui::PopID();
                        }
                        ImGui::EndTable();
                    };
                if (currentClip) {
                    ImGui::SeparatorText("Enable Keyframes");
                    for (const auto& keyframe :
                         currentClip
                             ->speakerFramingEnabledKeyframes) {
                        ImGui::PushID(
                            static_cast<int>(keyframe.frame));
                        ImGui::Text(
                            "Frame %lld: %s",
                            static_cast<long long>(keyframe.frame),
                            keyframe.enabled ? "Enabled" : "Disabled");
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Seek")) {
                            applyCommand(
                                shellState,
                                jcut::SeekToFrameCommand{
                                    currentClip->startFrame +
                                    static_cast<int>(keyframe.frame)});
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Remove")) {
                            applyCommand(
                                shellState,
                                jcut::RemoveClipKeyframeCommand{
                                    currentClip->id,
                                    jcut::EditorKeyframeChannel::
                                        SpeakerFramingEnabled,
                                    keyframe.frame});
                        }
                        ImGui::PopID();
                    }
                    drawFramingRows(
                        "SpeakerFramingBakedKeys",
                        "Baked Transform Keyframes",
                        currentClip->speakerFramingKeyframes,
                        jcut::EditorKeyframeChannel::
                            SpeakerFraming);
                    drawFramingRows(
                        "SpeakerFramingTargetKeys",
                        "Dynamic Target Keyframes",
                        currentClip
                            ->speakerFramingTargetKeyframes,
                        jcut::EditorKeyframeChannel::
                            SpeakerFramingTarget);
                }
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
                        markInspectorDeleteTargetForLastItem(
                            shellState,
                            InspectorDeleteTargetKind::ClipKeyframe,
                            currentClip->id,
                            jcut::EditorKeyframeChannel::Transform,
                            keyframe.frame);
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
        if (ImGui::BeginTabItem(
                "Transcript", nullptr, inspectorTabFlags("Transcript"))) {
            drawInspectorHeading("Transcript", snapshot, currentClip);
            const jcut::jobs::TranscriptionJobSnapshotCore
                transcriptionJob =
                    shellState->transcriptionJob.snapshot();
            ImGui::BeginDisabled(
                !currentClip || !currentClip->hasAudio ||
                transcriptionJob.active());
            if (ImGui::Button("Transcribe with WhisperX") &&
                currentClip) {
                startTranscriptionJob(shellState, *currentClip);
            }
            ImGui::EndDisabled();
            if (!transcriptionJob.status.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled(
                    "%s", transcriptionJob.status.c_str());
            }
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
                                markTranscriptDeleteTargetForLastItem(
                                    shellState,
                                    currentClip->id,
                                    transcript,
                                    row);
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
                    if (std::exchange(
                            shellState->transcriptDeletePopupRequested,
                            false)) {
                        ImGui::OpenPopup(
                            "Confirm Delete Transcript Word");
                    }
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
        if (ImGui::BeginTabItem(
                "Speakers", nullptr, inspectorTabFlags("Speakers"))) {
            drawInspectorHeading("Speakers", snapshot, currentClip);
            shellState->sectionAvatarDesiredKey.clear();
            shellState->sectionAvatarTracks.clear();
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
                    ImGui::Checkbox(
                        "Show Contiguous Speaker Sections",
                        &cache.speakerSectionsExpanded);
                    if (cache.speakerSectionsExpanded) {
                        int minimumWords =
                            std::clamp(
                                currentClip->
                                    speakerSectionMinimumWords,
                                0,
                                1000);
                        ImGui::SetNextItemWidth(120.0f);
                        if (ImGui::InputInt(
                                "Minimum Section Words",
                                &minimumWords,
                                1,
                                10)) {
                            applyCommand(
                                shellState,
                                jcut::
                                    SetClipSpeakerSectionMinimumWordsCommand{
                                        currentClip->id,
                                        minimumWords});
                        }
                        const std::vector<jcut::SpeakerSectionCore>
                            sections =
                                jcut::projectSpeakerSectionsCore(
                                    transcript.activeDocument->root(),
                                    minimumWords,
                                    30.0);
                        const std::string sectionClipIdentity =
                            currentClip->persistentId.empty()
                            ? std::to_string(currentClip->id)
                            : currentClip->persistentId;
                        std::vector<
                            std::vector<
                                jcut::SpeakerTrackAssignmentCore>>
                            sectionAssignmentsByIndex;
                        sectionAssignmentsByIndex.reserve(
                            sections.size());
                        std::vector<int> sectionAvatarTrackIds;
                        for (const auto& section : sections) {
                            auto assignments =
                                jcut::
                                    transcriptSpeakerTrackAssignmentsAtFrame(
                                        transcript.activeDocument->
                                            root(),
                                        sectionClipIdentity,
                                        section.speakerId,
                                        section.startFrame);
                            for (const auto& assignment :
                                 assignments) {
                                if (assignment.trackId >= 0 &&
                                    std::find(
                                        sectionAvatarTrackIds.begin(),
                                        sectionAvatarTrackIds.end(),
                                        assignment.trackId) ==
                                        sectionAvatarTrackIds.end()) {
                                    sectionAvatarTrackIds.push_back(
                                        assignment.trackId);
                                }
                            }
                            sectionAssignmentsByIndex.push_back(
                                std::move(assignments));
                        }
                        constexpr std::size_t
                            kMaximumSectionAvatarTracks = 24;
                        std::vector<jcut::FaceContinuityTrackCore>
                            sectionAvatarTracks;
                        for (const int trackId :
                             sectionAvatarTrackIds) {
                            const auto found = std::find_if(
                                cache.faceInspection.tracks.begin(),
                                cache.faceInspection.tracks.end(),
                                [&](const auto& track) {
                                    return track.trackId ==
                                        trackId;
                                });
                            if (found !=
                                cache.faceInspection.tracks.end()) {
                                sectionAvatarTracks.push_back(*found);
                                if (sectionAvatarTracks.size() ==
                                    kMaximumSectionAvatarTracks) {
                                    break;
                                }
                            }
                        }
                        if (!sectionAvatarTracks.empty()) {
                            const std::string avatarSourcePath =
                                pathString(
                                    resolvedClipSourcePath(
                                        *shellState,
                                        *currentClip));
                            std::string avatarKey =
                                transcript.activePath + "::" +
                                sectionClipIdentity + "::" +
                                avatarSourcePath;
                            for (const auto& track :
                                 sectionAvatarTracks) {
                                avatarKey += "::" +
                                    std::to_string(track.trackId) +
                                    ":" +
                                    std::to_string(
                                        track.firstFrame) +
                                    ":" +
                                    std::to_string(track.x) +
                                    ":" +
                                    std::to_string(track.y) +
                                    ":" +
                                    std::to_string(track.box);
                            }
                            shellState->
                                sectionAvatarDesiredKey =
                                    std::move(avatarKey);
                            shellState->
                                sectionAvatarSourcePath =
                                    avatarSourcePath;
                            shellState->sectionAvatarTracks =
                                sectionAvatarTracks;
                        } else {
                            shellState->
                                sectionAvatarDesiredKey.clear();
                            shellState->sectionAvatarTracks.clear();
                        }
                        ImGui::Text(
                            "%zu contiguous section%s",
                            sections.size(),
                            sections.size() == 1 ? "" : "s");
                        if (ImGui::BeginTable(
                                "SpeakerSections",
                                7,
                                ImGuiTableFlags_Borders |
                                    ImGuiTableFlags_RowBg |
                                    ImGuiTableFlags_Resizable |
                                    ImGuiTableFlags_ScrollX |
                                ImGuiTableFlags_ScrollY,
                                ImVec2(0.0f, 260.0f))) {
                            ImGui::TableSetupColumn(
                                "Faces",
                                ImGuiTableColumnFlags_WidthFixed,
                                80.0f);
                            ImGui::TableSetupColumn(
                                "Speaker",
                                ImGuiTableColumnFlags_WidthFixed,
                                110.0f);
                            ImGui::TableSetupColumn(
                                "Frames",
                                ImGuiTableColumnFlags_WidthFixed,
                                100.0f);
                            ImGui::TableSetupColumn(
                                "Tracks",
                                ImGuiTableColumnFlags_WidthFixed,
                                80.0f);
                            ImGui::TableSetupColumn(
                                "Words",
                                ImGuiTableColumnFlags_WidthFixed,
                                52.0f);
                            ImGui::TableSetupColumn(
                                "Transcript",
                                ImGuiTableColumnFlags_WidthStretch,
                                180.0f);
                            ImGui::TableSetupColumn(
                                "Actions",
                                ImGuiTableColumnFlags_WidthFixed,
                                370.0f);
                            ImGui::TableHeadersRow();
                            for (std::size_t sectionIndex = 0;
                                 sectionIndex < sections.size();
                                 ++sectionIndex) {
                                const auto& section =
                                    sections[sectionIndex];
                                ImGui::PushID(
                                    static_cast<int>(sectionIndex));
                                ImGui::TableNextRow();
                                ImGui::TableNextColumn();
                                const auto& sectionAssignments =
                                    sectionAssignmentsByIndex[
                                        sectionIndex];
                                bool drewSectionAvatar = false;
                                if (shellState->
                                        sectionAvatarTextureId != 0 &&
                                    shellState->
                                        sectionAvatarLoadedKey ==
                                        shellState->
                                            sectionAvatarDesiredKey &&
                                    shellState->
                                        sectionAvatarSize.width > 0) {
                                    for (const auto& assignment :
                                         sectionAssignments) {
                                        const auto found =
                                            std::find_if(
                                                sectionAvatarTracks.
                                                    begin(),
                                                sectionAvatarTracks.
                                                    end(),
                                                [&](const auto& track) {
                                                    return track.trackId ==
                                                        assignment.
                                                            trackId;
                                                });
                                        if (found ==
                                            sectionAvatarTracks.end()) {
                                            continue;
                                        }
                                        const std::size_t avatarIndex =
                                            static_cast<std::size_t>(
                                                std::distance(
                                                    sectionAvatarTracks.
                                                        begin(),
                                                    found));
                                        const auto avatarUv =
                                            jcut::
                                                faceAvatarStripUvCore(
                                                    avatarIndex,
                                                    shellState->
                                                        sectionAvatarSize.
                                                        width,
                                                    80,
                                                    2);
                                        if (!avatarUv.valid()) {
                                            continue;
                                        }
                                        if (drewSectionAvatar) {
                                            ImGui::SameLine(
                                                0.0f, 2.0f);
                                        }
                                        ImGui::Image(
                                            shellState->
                                                sectionAvatarTextureId,
                                            ImVec2(32.0f, 32.0f),
                                            ImVec2(
                                                static_cast<float>(
                                                    avatarUv.left),
                                                0.0f),
                                            ImVec2(
                                                static_cast<float>(
                                                    avatarUv.right),
                                                1.0f));
                                        if (ImGui::
                                                IsItemHovered()) {
                                            ImGui::SetTooltip(
                                                "Continuity-track avatar T%d at source frame %lld",
                                                found->trackId,
                                                static_cast<
                                                    long long>(
                                                    found->
                                                        firstFrame));
                                        }
                                        drewSectionAvatar = true;
                                    }
                                }
                                if (!drewSectionAvatar) {
                                    ImGui::TextDisabled(
                                        sectionAssignments.empty()
                                        ? "-"
                                        : (shellState->
                                                sectionAvatarRunning
                                            ? "..."
                                            : "T"));
                                }
                                ImGui::TableNextColumn();
                                if (ImGui::Selectable(
                                        section.displayLabel.c_str(),
                                        cache.selectedSpeakerId ==
                                            section.speakerId,
                                        ImGuiSelectableFlags_AllowOverlap)) {
                                    cache.selectedSpeakerId =
                                        section.speakerId;
                                }
                                ImGui::TableNextColumn();
                                ImGui::Text(
                                    "%lld-%lld",
                                    static_cast<long long>(
                                        section.startFrame),
                                    static_cast<long long>(
                                        section.endFrame));
                                ImGui::TableNextColumn();
                                std::string trackLabels;
                                for (const auto& assignment :
                                     sectionAssignments) {
                                    if (!trackLabels.empty()) {
                                        trackLabels += ", ";
                                    }
                                    trackLabels += assignment.trackId >= 0
                                        ? "T" +
                                            std::to_string(
                                                assignment.trackId)
                                        : assignment.streamId;
                                }
                                ImGui::TextUnformatted(
                                    trackLabels.empty()
                                    ? "-"
                                    : trackLabels.c_str());
                                if (!sectionAssignments.empty() &&
                                    std::abs(
                                        sectionAssignments.front().
                                            rotationDegrees) > 0.0001) {
                                    ImGui::SetItemTooltip(
                                        "Section rotation %.1f degrees",
                                        sectionAssignments.front().
                                            rotationDegrees);
                                }
                                ImGui::TableNextColumn();
                                ImGui::Text(
                                    "%zu", section.wordCount);
                                ImGui::TableNextColumn();
                                std::string snippet;
                                for (const std::string& word :
                                     section.snippetWords) {
                                    if (!snippet.empty()) snippet += ' ';
                                    snippet += word;
                                }
                                if (section.wordCount >
                                    section.snippetWords.size()) {
                                    snippet += " ...";
                                }
                                ImGui::TextUnformatted(
                                    snippet.c_str());
                                ImGui::TableNextColumn();
                                if (ImGui::SmallButton("View")) {
                                    applyCommand(
                                        shellState,
                                        jcut::SeekToFrameCommand{
                                            static_cast<int>(
                                                jcut::
                                                    faceTrackAnchorTimelineFrame(
                                                        section.startFrame,
                                                        currentClip->
                                                            sourceInFrame,
                                                        currentClip->
                                                            startFrame,
                                                        currentClip->
                                                            durationFrames,
                                                        currentClip->
                                                            playbackRate))});
                                }
                                ImGui::SameLine();
                                ImGui::BeginDisabled(
                                    !transcript.activeCutMutable);
                                if (ImGui::SmallButton("Skip") &&
                                    transcript.activeDocument) {
                                    nlohmann::json root =
                                        transcript.activeDocument->root();
                                    std::string error;
                                    if (jcut::
                                            setSpeakerSectionSkippedCore(
                                                &root,
                                                section.speakerId,
                                                section.startFrame,
                                                section.endFrame,
                                                true,
                                                30.0,
                                                &error)) {
                                        saveTranscriptMutation(
                                            shellState,
                                            &cache,
                                            std::move(root));
                                    } else if (!error.empty()) {
                                        cache.mutationError =
                                            std::move(error);
                                    }
                                }
                                ImGui::SameLine();
                                if (ImGui::SmallButton("Unskip") &&
                                    transcript.activeDocument) {
                                    nlohmann::json root =
                                        transcript.activeDocument->root();
                                    std::string error;
                                    if (jcut::
                                            setSpeakerSectionSkippedCore(
                                                &root,
                                                section.speakerId,
                                                section.startFrame,
                                                section.endFrame,
                                                false,
                                                30.0,
                                                &error)) {
                                        saveTranscriptMutation(
                                            shellState,
                                            &cache,
                                            std::move(root));
                                    } else if (!error.empty()) {
                                        cache.mutationError =
                                            std::move(error);
                                    }
                                }
                                ImGui::SameLine();
                                ImGui::BeginDisabled(
                                    cache.selectedFaceTrackIds.empty());
                                if (ImGui::SmallButton("Assign") &&
                                    transcript.activeDocument) {
                                    std::vector<
                                        jcut::
                                            TranscriptTrackAssignmentAnchor>
                                        anchors;
                                    for (const auto& track :
                                         cache.faceInspection.tracks) {
                                        if (std::find(
                                                cache.
                                                    selectedFaceTrackIds.
                                                    begin(),
                                                cache.
                                                    selectedFaceTrackIds.
                                                    end(),
                                                track.trackId) ==
                                            cache.
                                                selectedFaceTrackIds.
                                                end()) {
                                            continue;
                                        }
                                        anchors.push_back({
                                            track.trackId,
                                            track.streamId,
                                            std::max<std::int64_t>(
                                                0,
                                                track.firstFrame),
                                            track.x,
                                            track.y,
                                            track.box});
                                    }
                                    nlohmann::json root =
                                        transcript.activeDocument->root();
                                    std::string error;
                                    if (jcut::
                                            setSpeakerSectionTrackAssignmentsCore(
                                                &root,
                                                sectionClipIdentity,
                                                section.speakerId,
                                                section.startFrame,
                                                section.endFrame,
                                                section.wordCount,
                                                anchors,
                                                false,
                                                "contiguous_section_picker",
                                                {},
                                                &error)) {
                                        saveTranscriptMutation(
                                            shellState,
                                            &cache,
                                            std::move(root));
                                    } else if (!error.empty()) {
                                        cache.mutationError =
                                            std::move(error);
                                    }
                                }
                                ImGui::EndDisabled();
                                if (ImGui::IsItemHovered(
                                        ImGuiHoveredFlags_AllowWhenDisabled)) {
                                    ImGui::SetTooltip(
                                        "Add the continuity tracks selected below to this section.");
                                }
                                ImGui::SameLine();
                                ImGui::BeginDisabled(
                                    sectionAssignments.empty());
                                if (ImGui::SmallButton("Clear") &&
                                    transcript.activeDocument) {
                                    nlohmann::json root =
                                        transcript.activeDocument->root();
                                    std::string error;
                                    if (jcut::
                                            setSpeakerSectionTrackAssignmentsCore(
                                                &root,
                                                sectionClipIdentity,
                                                section.speakerId,
                                                section.startFrame,
                                                section.endFrame,
                                                section.wordCount,
                                                {},
                                                true,
                                                "contiguous_section_picker",
                                                {},
                                                &error)) {
                                        saveTranscriptMutation(
                                            shellState,
                                            &cache,
                                            std::move(root));
                                    } else if (!error.empty()) {
                                        cache.mutationError =
                                            std::move(error);
                                    }
                                }
                                ImGui::EndDisabled();
                                ImGui::SameLine();
                                if (ImGui::SmallButton("Export")) {
                                    const auto ranges =
                                        jcut::
                                            editorTimelineRangesForTranscriptSection(
                                                snapshot,
                                                *currentClip,
                                                section.startFrame,
                                                section.endFrame);
                                    if (ranges.empty()) {
                                        cache.mutationError =
                                            "Section could not be mapped to timeline frames.";
                                    } else {
                                        const jcut::CommandResult
                                            rangeResult =
                                                applyCommand(
                                                    shellState,
                                                    jcut::
                                                        SetExportRangesCommand{
                                                            ranges});
                                        if (rangeResult.applied) {
                                            shellState->
                                                focusInspectorOutputRequested =
                                                    true;
                                            if (shellState->
                                                    exportOutputPath[0] ==
                                                '\0') {
                                                shellState->
                                                    statusMessage =
                                                        "section range loaded; choose an Output path and press Export";
                                            } else if (
                                                requestExportRender(
                                                    shellState)) {
                                                shellState->
                                                    statusMessage =
                                                        "speaker section export started";
                                            } else {
                                                shellState->
                                                    statusMessage =
                                                        "export already running";
                                            }
                                        } else {
                                            cache.mutationError =
                                                rangeResult.message;
                                        }
                                    }
                                }
                                if (ImGui::IsItemHovered()) {
                                    ImGui::SetTooltip(
                                        "Export this section with the current Output settings.");
                                }
                                ImGui::SameLine();
                                if (ImGui::SmallButton("Options") &&
                                    transcript.activeDocument) {
                                    cache.
                                        speakerSectionOptionsSpeakerId =
                                            section.speakerId;
                                    cache.
                                        speakerSectionOptionsStartFrame =
                                            section.startFrame;
                                    cache.
                                        speakerSectionOptionsEndFrame =
                                            section.endFrame;
                                    cache.
                                        speakerSectionOptionsWordCount =
                                            section.wordCount;
                                    cache.speakerSectionOptionsDraft =
                                        jcut::speakerSectionOptionsCore(
                                            transcript.activeDocument->
                                                root(),
                                            sectionClipIdentity,
                                            section.speakerId,
                                            section.startFrame,
                                            section.endFrame);
                                    cache.
                                        speakerSectionOptionsPopupRequested =
                                            true;
                                }
                                ImGui::EndDisabled();
                                ImGui::PopID();
                            }
                            ImGui::EndTable();
                        }
                        bool exportBusy = false;
                        {
                            std::lock_guard<std::mutex> lock(
                                shellState->exportMutex);
                            exportBusy =
                                shellState->exportRunning ||
                                shellState->exportRequested;
                        }
                        ImGui::BeginDisabled(
                            sections.empty() || exportBusy);
                        if (ImGui::Button(
                                "Export All Qualifying Sections")) {
                            shellState->
                                focusInspectorOutputRequested =
                                    true;
                            if (shellState->
                                    exportOutputPath[0] == '\0') {
                                shellState->statusMessage =
                                    "choose an Output path first; its directory and format are used for section files";
                            } else {
                                std::size_t skipped = 0;
                                const std::size_t queued =
                                    requestSpeakerSectionExportBatch(
                                        shellState,
                                        snapshot,
                                        *currentClip,
                                        transcript.activeDocument->
                                            root(),
                                        sections,
                                        &skipped);
                                if (queued > 0) {
                                    shellState->statusMessage =
                                        "queued " +
                                        std::to_string(queued) +
                                        " speaker section export" +
                                        (queued == 1 ? "" : "s") +
                                        (skipped > 0
                                            ? "; skipped " +
                                                std::to_string(
                                                    skipped) +
                                                " existing, duplicate, or unmapped"
                                            : "");
                                } else {
                                    shellState->statusMessage =
                                        "no speaker section exports were queued";
                                }
                            }
                        }
                        ImGui::EndDisabled();
                        if (ImGui::IsItemHovered(
                                ImGuiHoveredFlags_AllowWhenDisabled)) {
                            ImGui::SetTooltip(
                                "Coalesce adjacent same-speaker rows and export each remaining section to the configured Output directory.");
                        }
                        if (std::exchange(
                                cache.
                                    speakerSectionOptionsPopupRequested,
                                false)) {
                            ImGui::OpenPopup(
                                "Speaker Section Options");
                        }
                        if (ImGui::BeginPopupModal(
                                "Speaker Section Options",
                                nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
                            auto& options =
                                cache.speakerSectionOptionsDraft;
                            ImGui::Text(
                                "%s | frames %lld-%lld",
                                cache.
                                    speakerSectionOptionsSpeakerId.
                                    c_str(),
                                static_cast<long long>(
                                    cache.
                                        speakerSectionOptionsStartFrame),
                                static_cast<long long>(
                                    cache.
                                        speakerSectionOptionsEndFrame));
                            ImGui::SetNextItemWidth(180.0f);
                            ImGui::InputDouble(
                                "Rotation (degrees)",
                                &options.rotationDegrees,
                                0.1,
                                1.0,
                                "%.1f");
                            options.rotationDegrees = std::clamp(
                                options.rotationDegrees,
                                -180.0,
                                180.0);
                            ImGui::SeparatorText("Grading");
                            ImGui::Checkbox(
                                "Add Grading Keyframes",
                                &options.gradingEnabled);
                            ImGui::InputDouble(
                                "Brightness",
                                &options.gradingBrightness,
                                0.01,
                                0.1,
                                "%.3f");
                            ImGui::InputDouble(
                                "Contrast",
                                &options.gradingContrast,
                                0.05,
                                0.25,
                                "%.3f");
                            ImGui::InputDouble(
                                "Saturation",
                                &options.gradingSaturation,
                                0.05,
                                0.25,
                                "%.3f");
                            ImGui::SeparatorText("Mask Override");
                            ImGui::Checkbox(
                                "Enable Mask Override",
                                &options.maskEnabled);
                            ImGui::InputDouble(
                                "Mask Opacity",
                                &options.maskOpacity,
                                0.01,
                                0.1,
                                "%.3f");
                            ImGui::InputDouble(
                                "Mask Feather",
                                &options.maskFeather,
                                0.5,
                                5.0,
                                "%.2f");
                            ImGui::InputDouble(
                                "Mask Blur",
                                &options.maskBlur,
                                0.5,
                                5.0,
                                "%.2f");
                            ImGui::InputDouble(
                                "Mask Dilate",
                                &options.maskDilate,
                                0.5,
                                5.0,
                                "%.2f");
                            ImGui::Checkbox(
                                "Invert Section Mask",
                                &options.maskInvert);
                            ImGui::BeginDisabled(
                                !transcript.activeCutMutable);
                            if (ImGui::Button("Apply") &&
                                transcript.activeDocument) {
                                nlohmann::json root =
                                    transcript.activeDocument->root();
                                std::string error;
                                if (jcut::setSpeakerSectionOptionsCore(
                                        &root,
                                        sectionClipIdentity,
                                        cache.
                                            speakerSectionOptionsSpeakerId,
                                        cache.
                                            speakerSectionOptionsStartFrame,
                                        cache.
                                            speakerSectionOptionsEndFrame,
                                        cache.
                                            speakerSectionOptionsWordCount,
                                        options,
                                        {},
                                        &error)) {
                                    {
                                        std::lock_guard<std::mutex>
                                            lock(
                                                shellState->
                                                    runtimeMutex);
                                        shellState->runtime.
                                            beginHistoryTransaction();
                                    }
                                    const bool transcriptSaved =
                                        saveTranscriptMutation(
                                            shellState,
                                            &cache,
                                            std::move(root));
                                    bool gradingApplied = true;
                                    if (transcriptSaved &&
                                        options.gradingEnabled) {
                                        const std::int64_t
                                            maximumLocalFrame =
                                                std::max(
                                                    0,
                                                    currentClip->
                                                        durationFrames -
                                                        1);
                                        const std::int64_t
                                            startLocalFrame =
                                                std::clamp<
                                                    std::int64_t>(
                                                    cache.
                                                        speakerSectionOptionsStartFrame -
                                                        std::max<
                                                            std::int64_t>(
                                                            0,
                                                            currentClip->
                                                                sourceInFrame),
                                                    0,
                                                    maximumLocalFrame);
                                        const std::int64_t
                                            endLocalFrame =
                                                std::clamp<
                                                    std::int64_t>(
                                                    cache.
                                                        speakerSectionOptionsEndFrame -
                                                        std::max<
                                                            std::int64_t>(
                                                            0,
                                                            currentClip->
                                                                sourceInFrame),
                                                    0,
                                                    maximumLocalFrame);
                                        std::vector<std::int64_t>
                                            frames{startLocalFrame};
                                        if (endLocalFrame !=
                                            startLocalFrame) {
                                            frames.push_back(
                                                endLocalFrame);
                                        }
                                        for (const std::int64_t frame :
                                             frames) {
                                            jcut::
                                                EditorGradingKeyframe
                                                    keyframe =
                                                        jcut::
                                                            evaluateEditorClipGradingAtLocalFrame(
                                                                *currentClip,
                                                                frame);
                                            keyframe.frame = frame;
                                            keyframe.brightness =
                                                std::clamp(
                                                    options.
                                                        gradingBrightness,
                                                    -10.0,
                                                    10.0);
                                            keyframe.contrast =
                                                std::clamp(
                                                    options.
                                                        gradingContrast,
                                                    0.05,
                                                    10.0);
                                            keyframe.saturation =
                                                std::clamp(
                                                    options.
                                                        gradingSaturation,
                                                    0.0,
                                                    10.0);
                                            keyframe.
                                                linearInterpolation =
                                                    true;
                                            const auto result =
                                                applyCommand(
                                                    shellState,
                                                    jcut::
                                                        UpsertGradingKeyframeCommand{
                                                            currentClip->
                                                                id,
                                                            keyframe});
                                            gradingApplied &=
                                                result.applied;
                                        }
                                    }
                                    {
                                        std::lock_guard<std::mutex>
                                            lock(
                                                shellState->
                                                    runtimeMutex);
                                        shellState->runtime.
                                            endHistoryTransaction();
                                    }
                                    if (transcriptSaved &&
                                        gradingApplied) {
                                        ImGui::CloseCurrentPopup();
                                    } else if (
                                        transcriptSaved &&
                                        !gradingApplied) {
                                        cache.mutationError =
                                            "Section options were saved, "
                                            "but grading keyframes could "
                                            "not be applied.";
                                    }
                                } else if (!error.empty()) {
                                    cache.mutationError =
                                        std::move(error);
                                } else {
                                    ImGui::CloseCurrentPopup();
                                }
                            }
                            ImGui::EndDisabled();
                            ImGui::SameLine();
                            if (ImGui::Button("Cancel")) {
                                ImGui::CloseCurrentPopup();
                            }
                            ImGui::EndPopup();
                        }
                    }
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
                    ImGui::BeginDisabled(
                        !transcript.activeDocument ||
                        shellState->aiTaskRunning ||
                        !shellState->featureAiSpeakerCleanup ||
                        !shellState->aiAccount.aiEnabled);
                    if (ImGui::Button("Mine Profiles with Cloud AI") &&
                        transcript.activeDocument) {
                        startCloudSpeakerMining(
                            shellState,
                            transcript.activeDocument->root(),
                            cache.sourceKey,
                            snapshot);
                    }
                    ImGui::EndDisabled();
                    if (shellState->aiTaskRunning &&
                        shellState->aiTaskPurpose ==
                            AiTaskPurpose::CloudSpeakerMining) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("Mining...");
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
                    const fs::path faceSourceMediaPath =
                        resolvedClipSourcePath(*shellState, *currentClip);
                    const std::string faceClipIdentity =
                        currentClip->persistentId.empty()
                            ? std::to_string(currentClip->id)
                            : currentClip->persistentId;
                    const std::string faceOutputDirectory =
                        jcut::faceProcessingSidecarDirectory(
                            pathString(faceSourceMediaPath),
                            faceClipIdentity);
                    const jcut::FaceProcessingLaunchControl
                        savedFaceLaunchControl =
                            jcut::loadFaceProcessingLaunchControl(
                                faceOutputDirectory);
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
                    if (savedFaceLaunchControl.hasRecommendation) {
                        ImGui::TextDisabled(
                            "Saved benchmark recommendation: %d worker(s), %d slot(s)",
                            savedFaceLaunchControl.detectorWorkers,
                            savedFaceLaunchControl.detectorPipelineSlots);
                        if (ImGui::SmallButton(
                                "Apply Saved Topology Recommendation")) {
                            cache.faceJobWorkers =
                                savedFaceLaunchControl.detectorWorkers;
                            cache.faceJobPipelineSlots =
                                savedFaceLaunchControl.detectorPipelineSlots;
                        }
                    } else if (!savedFaceLaunchControl.error.empty()) {
                        ImGui::TextDisabled(
                            "%s", savedFaceLaunchControl.error.c_str());
                    }
                    ImGui::Checkbox(
                        "Benchmark topology before launch",
                        &cache.faceJobBenchmarkTopology);
                    ImGui::Checkbox(
                        "Apply selected clip grading during detection",
                        &cache.faceJobApplyClipGrading);
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
                    ImGui::Checkbox(
                        "Generator control window",
                        &cache.faceJobControlWindow);
                    ImGui::SameLine();
                    ImGui::Checkbox(
                        "Live preview window",
                        &cache.faceJobLivePreview);
                    ImGui::Checkbox(
                        "Restart from scratch (clear resume checkpoint)",
                        &cache.faceJobRestartFromScratch);
                    const fs::path configuredProxyPath =
                        resolvedClipProxyPath(*shellState, *currentClip);
                    const bool faceProxyAvailable =
                        !currentClip->proxyPath.empty() &&
                        isImportableMediaPath(configuredProxyPath);
                    ImGui::BeginDisabled(!faceProxyAvailable);
                    ImGui::Checkbox(
                        "Use proxy media as detector input",
                        &cache.faceJobUseProxySource);
                    ImGui::EndDisabled();
                    if (!faceProxyAvailable) {
                        cache.faceJobUseProxySource = false;
                        ImGui::SameLine();
                        ImGui::TextDisabled("(no playable proxy configured)");
                    }
                    if (ImGui::Button("Generate Detection + Continuity")) {
                        const fs::path mediaPath = cache.faceJobUseProxySource
                            ? configuredProxyPath
                            : faceSourceMediaPath;
                        jcut::FaceProcessingJobRequest request;
                        request.executablePath =
                            pathString(executableDirPath() /
                                       "jcut_vulkan_facedetections_offscreen");
                        request.mediaPath = pathString(mediaPath);
                        request.transcriptPath = transcript.activePath;
                        request.clipId = faceClipIdentity;
                        request.outputDirectory = faceOutputDirectory;
                        request.detectorSettingsPath =
                            pathString(faceSourceMediaPath.parent_path() /
                                (faceSourceMediaPath.stem().string() +
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
                        request.controlWindow =
                            cache.faceJobControlWindow;
                        request.livePreview =
                            cache.faceJobLivePreview;
                        request.restartFromScratch =
                            cache.faceJobRestartFromScratch;
                        request.benchmarkTopology =
                            cache.faceJobBenchmarkTopology;
                        request.applyClipGrading =
                            cache.faceJobApplyClipGrading;
                        if (request.applyClipGrading) {
                            request.clipJson =
                                jcut::toLegacyClipJson(*currentClip).dump();
                        }
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
                    std::vector<jcut::FaceContinuityTrackCore>
                        referenceTracks;
                    constexpr std::size_t kMaxReferenceTracks = 8;
                    referenceTracks.reserve(std::min(
                        cache.selectedFaceTrackIds.size(),
                        kMaxReferenceTracks));
                    for (const int selectedTrackId :
                         cache.selectedFaceTrackIds) {
                        const auto found = std::find_if(
                            cache.faceInspection.tracks.begin(),
                            cache.faceInspection.tracks.end(),
                            [&](const auto& track) {
                                return track.trackId ==
                                    selectedTrackId;
                            });
                        if (found !=
                            cache.faceInspection.tracks.end()) {
                            referenceTracks.push_back(*found);
                            if (referenceTracks.size() ==
                                kMaxReferenceTracks) {
                                break;
                            }
                        }
                    }
                    if (!referenceTracks.empty()) {
                        std::string referenceKey = artifactContext;
                        for (const auto& track : referenceTracks) {
                            referenceKey += "::" +
                                std::to_string(track.trackId) + ":" +
                                std::to_string(track.firstFrame) + ":" +
                                std::to_string(track.x) + ":" +
                                std::to_string(track.y) + ":" +
                                std::to_string(track.box);
                        }
                        shellState->faceReferenceDesiredKey =
                            std::move(referenceKey);
                        shellState->faceReferenceSourcePath =
                            pathString(faceSourceMediaPath);
                        shellState->faceReferenceTracks =
                            referenceTracks;
                    } else {
                        shellState->faceReferenceDesiredKey.clear();
                        shellState->faceReferenceTracks.clear();
                    }
                    if (!referenceTracks.empty() &&
                        shellState->faceReferenceTextureId != 0 &&
                        shellState->faceReferenceLoadedKey ==
                            shellState->faceReferenceDesiredKey) {
                        ImGui::SeparatorText("Selected References");
                        const float naturalWidth =
                            static_cast<float>(
                                shellState->faceReferenceSize.width) *
                            0.75f;
                        const float displayWidth = std::min(
                            ImGui::GetContentRegionAvail().x,
                            naturalWidth);
                        const float displayHeight =
                            shellState->faceReferenceSize.width > 0
                            ? displayWidth *
                                static_cast<float>(
                                    shellState->
                                        faceReferenceSize.height) /
                                static_cast<float>(
                                    shellState->
                                        faceReferenceSize.width)
                            : 120.0f;
                        ImGui::Image(
                            shellState->faceReferenceTextureId,
                            ImVec2(displayWidth, displayHeight));
                        if (ImGui::BeginTable(
                                "##reference_diagnostics",
                                4,
                                ImGuiTableFlags_SizingStretchProp |
                                    ImGuiTableFlags_RowBg)) {
                            ImGui::TableSetupColumn("Track");
                            ImGui::TableSetupColumn("Frame");
                            ImGui::TableSetupColumn("Center / box");
                            ImGui::TableSetupColumn("Score");
                            ImGui::TableHeadersRow();
                            for (const auto& track : referenceTracks) {
                                ImGui::TableNextRow();
                                ImGui::TableNextColumn();
                                ImGui::Text("T%d", track.trackId);
                                ImGui::TableNextColumn();
                                ImGui::Text(
                                    "%lld",
                                    static_cast<long long>(
                                        track.firstFrame));
                                ImGui::TableNextColumn();
                                ImGui::Text(
                                    "%.3f, %.3f / %.3f",
                                    track.x,
                                    track.y,
                                    track.box);
                                ImGui::TableNextColumn();
                                ImGui::Text("%.3f", track.score);
                            }
                            ImGui::EndTable();
                        }
                        if (cache.selectedFaceTrackIds.size() >
                            kMaxReferenceTracks) {
                            ImGui::TextDisabled(
                                "Showing the first %zu of %zu selected tracks.",
                                kMaxReferenceTracks,
                                cache.selectedFaceTrackIds.size());
                        }
                    } else if (!referenceTracks.empty() &&
                               shellState->
                                   faceReferenceRunning) {
                        ImGui::TextDisabled(
                            "Decoding selected face references...");
                    } else if (!referenceTracks.empty() &&
                               !shellState->
                                   faceReferenceError.empty()) {
                        ImGui::TextDisabled(
                            "%s",
                            shellState->
                                faceReferenceError.c_str());
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
                        "The selected reference track is outlined in Program and shown as a "
                        "shared-policy face crop above.");
                }
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(
                "Properties", nullptr, inspectorTabFlags("Properties"))) {
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
            {
                static constexpr const char* treatmentLabels[] = {
                    "Preserve Pitch",
                    "Rubber Band at Any Speed",
                    "Harmonic Speech Isolation"};
                const jcut::EditorAudioTreatment treatmentValues[] = {
                    jcut::EditorAudioTreatment::PreservePitch,
                    jcut::EditorAudioTreatment::RubberBand,
                    jcut::EditorAudioTreatment::HarmonicSpeechIsolation};
                int treatmentIndex = 0;
                for (int index = 0; index < 3; ++index) {
                    if (snapshot.audioTreatment == treatmentValues[index]) {
                        treatmentIndex = index;
                    }
                }
                if (ImGui::Combo(
                        "Preview Audio Treatment",
                        &treatmentIndex,
                        treatmentLabels,
                        3)) {
                    applyCommand(
                        shellState,
                        jcut::SetAudioTreatmentCommand{
                            treatmentValues[treatmentIndex]});
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(
                        "Harmonic Speech Isolation uses two Rubber Band "
                        "stages and may buffer while the processed source "
                        "is prepared.");
                }
            }
            if (ImGui::CollapsingHeader(
                    "Master Dynamics",
                    ImGuiTreeNodeFlags_DefaultOpen)) {
                jcut::audio::DynamicsSettingsCore dynamics =
                    snapshot.audioDynamics;
                bool dynamicsChanged = false;
                const auto sliderDouble =
                    [&](const char* label,
                        double* value,
                        float minimum,
                        float maximum,
                        const char* format) {
                        float edited = static_cast<float>(*value);
                        const bool changed = ImGui::SliderFloat(
                            label, &edited, minimum, maximum, format);
                        beginRuntimeHistoryTransactionForLastItem(
                            shellState);
                        if (changed) *value = edited;
                        return changed;
                    };
                dynamicsChanged |= ImGui::Checkbox(
                    "Amplify", &dynamics.amplifyEnabled);
                if (dynamics.amplifyEnabled) {
                    dynamicsChanged |= sliderDouble(
                        "Amplify dB",
                        &dynamics.amplifyDb,
                        -24.0f,
                        24.0f,
                        "%.1f dB");
                }
                dynamicsChanged |= ImGui::Checkbox(
                    "Normalize", &dynamics.normalizeEnabled);
                if (dynamics.normalizeEnabled) {
                    dynamicsChanged |= sliderDouble(
                        "Normalize Target",
                        &dynamics.normalizeTargetDb,
                        -24.0f,
                        0.0f,
                        "%.1f dBFS");
                }
                dynamicsChanged |= ImGui::Checkbox(
                    "Transcript Normalize",
                    &dynamics.transcriptNormalizeEnabled);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(
                        "Normalize each active transcript word toward "
                        "-0.45 dBFS, capped at 2.5x gain.");
                }
                dynamicsChanged |= ImGui::Checkbox(
                    "Selective Normalize",
                    &dynamics.selectiveNormalizeEnabled);
                if (dynamics.selectiveNormalizeEnabled) {
                    dynamicsChanged |= sliderDouble(
                        "Minimum Segment",
                        &dynamics.selectiveNormalizeMinSegmentSeconds,
                        0.1f,
                        30.0f,
                        "%.1f s");
                    dynamicsChanged |= sliderDouble(
                        "Selective Peak",
                        &dynamics.selectiveNormalizePeakDb,
                        -36.0f,
                        0.0f,
                        "%.1f dBFS");
                    dynamicsChanged |= ImGui::SliderInt(
                        "Selective Passes",
                        &dynamics.selectiveNormalizePasses,
                        1,
                        8);
                }
                dynamicsChanged |= ImGui::Checkbox(
                    "Peak Reduction",
                    &dynamics.peakReductionEnabled);
                if (dynamics.peakReductionEnabled) {
                    dynamicsChanged |= sliderDouble(
                        "Peak Threshold",
                        &dynamics.peakThresholdDb,
                        -24.0f,
                        0.0f,
                        "%.1f dBFS");
                }
                dynamicsChanged |= ImGui::Checkbox(
                    "Compressor", &dynamics.compressorEnabled);
                if (dynamics.compressorEnabled) {
                    dynamicsChanged |= sliderDouble(
                        "Compressor Threshold",
                        &dynamics.compressorThresholdDb,
                        -30.0f,
                        -1.0f,
                        "%.1f dBFS");
                    dynamicsChanged |= sliderDouble(
                        "Compressor Ratio",
                        &dynamics.compressorRatio,
                        1.0f,
                        20.0f,
                        "%.1f:1");
                }
                dynamicsChanged |= ImGui::Checkbox(
                    "Soft Clip", &dynamics.softClipEnabled);
                dynamicsChanged |= ImGui::Checkbox(
                    "Limiter", &dynamics.limiterEnabled);
                if (dynamics.limiterEnabled) {
                    dynamicsChanged |= sliderDouble(
                        "Limiter Threshold",
                        &dynamics.limiterThresholdDb,
                        -12.0f,
                        0.0f,
                        "%.1f dBFS");
                }
                dynamicsChanged |= ImGui::Checkbox(
                    "Stereo to Mono",
                    &dynamics.stereoToMonoEnabled);
                if (dynamicsChanged) {
                    applyCommand(
                        shellState,
                        jcut::SetAudioDynamicsCommand{dynamics});
                }
            }
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
        if (ImGui::BeginTabItem(
                "Jobs", nullptr, inspectorTabFlags("Jobs"))) {
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
            const jcut::masks::PromptMaskJobSnapshot promptMaskJob =
                shellState->promptMaskJob.snapshot();
            const jcut::jobs::TranscriptionJobSnapshotCore
                transcriptionJob =
                    shellState->transcriptionJob.snapshot();
            const jcut::jobs::BiRefNetJobSnapshotCore birefnetJob =
                shellState->birefnetJob.snapshot();
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
            const auto promptMaskStateLabel =
                [](jcut::masks::PromptMaskJobSnapshot::State state) {
                    using State =
                        jcut::masks::PromptMaskJobSnapshot::State;
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
            const auto processStateLabel =
                [](jcut::jobs::ProcessJobSnapshotCore::State state) {
                    using State =
                        jcut::jobs::ProcessJobSnapshotCore::State;
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
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted("SAM3 prompt mask");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(
                    promptMaskStateLabel(promptMaskJob.state));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(
                    promptMaskJob.active()
                        ? "external worker"
                        : (promptMaskJob.exitCode >= 0
                               ? ("exit " +
                                  std::to_string(
                                      promptMaskJob.exitCode))
                                     .c_str()
                               : "-"));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted("WhisperX transcription");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(
                    processStateLabel(transcriptionJob.state));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(
                    transcriptionJob.outputReady
                        ? "transcript ready"
                        : (transcriptionJob.active()
                               ? "external worker"
                               : (transcriptionJob.exitCode >= 0
                                      ? ("exit " +
                                         std::to_string(
                                             transcriptionJob.exitCode))
                                            .c_str()
                                      : "-")));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted("BiRefNet alpha");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(
                    processStateLabel(birefnetJob.state));
                ImGui::TableNextColumn();
                if (birefnetJob.totalFrames > 0) {
                    ImGui::Text(
                        "%lld / %lld (%.1f%%)",
                        static_cast<long long>(
                            birefnetJob.currentFrame),
                        static_cast<long long>(
                            birefnetJob.totalFrames),
                        birefnetJob.percent);
                } else {
                    ImGui::TextUnformatted(
                        birefnetJob.outputReady
                            ? "alpha ready"
                            : (birefnetJob.active()
                                   ? "external worker"
                                   : "-"));
                }
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
            ImGui::SameLine();
            ImGui::BeginDisabled(!promptMaskJob.active());
            if (ImGui::Button("Cancel Prompt Mask")) {
                shellState->promptMaskJob.cancel();
                shellState->statusMessage =
                    "prompt-mask cancellation requested";
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!transcriptionJob.active());
            if (ImGui::Button("Cancel Transcription")) {
                shellState->transcriptionJob.cancel();
                shellState->statusMessage =
                    "transcription cancellation requested";
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!birefnetJob.active());
            if (ImGui::Button("Cancel BiRefNet")) {
                shellState->birefnetJob.cancel();
                shellState->statusMessage =
                    "BiRefNet cancellation requested";
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
            if (!promptMaskJob.status.empty()) {
                ImGui::TextWrapped(
                    "Prompt mask: %s",
                    promptMaskJob.status.c_str());
            }
            if (!promptMaskJob.selectedMaskPath.empty()) {
                ImGui::TextWrapped(
                    "Prompt-mask output: %s",
                    promptMaskJob.selectedMaskPath.c_str());
            }
            if (!promptMaskJob.manifestPath.empty()) {
                ImGui::TextWrapped(
                    "Prompt-mask manifest: %s",
                    promptMaskJob.manifestPath.c_str());
            }
            if (!promptMaskJob.logPath.empty()) {
                ImGui::TextWrapped(
                    "Prompt-mask log: %s",
                    promptMaskJob.logPath.c_str());
            }
            if (!transcriptionJob.status.empty()) {
                ImGui::TextWrapped(
                    "Transcription: %s",
                    transcriptionJob.status.c_str());
            }
            if (!transcriptionJob.outputTranscriptPath.empty()) {
                ImGui::TextWrapped(
                    "Transcript output: %s",
                    transcriptionJob.outputTranscriptPath.c_str());
            }
            if (transcriptionJob.active()) {
                ImGui::InputText(
                    "Transcription stdin",
                    &shellState->transcriptionStdinDraft);
                ImGui::SameLine();
                if (ImGui::Button("Send stdin")) {
                    std::string stdinError;
                    if (shellState->transcriptionJob.writeStdin(
                            shellState->transcriptionStdinDraft,
                            &stdinError)) {
                        shellState->transcriptionStdinDraft.clear();
                    } else {
                        shellState->statusMessage =
                            std::move(stdinError);
                    }
                }
            }
            if (!birefnetJob.status.empty()) {
                ImGui::TextWrapped(
                    "BiRefNet: %s", birefnetJob.status.c_str());
            }
            if (!birefnetJob.outputDirectory.empty()) {
                ImGui::TextWrapped(
                    "BiRefNet output: %s",
                    birefnetJob.outputDirectory.c_str());
            }
            if (!birefnetJob.livePreviewPath.empty()) {
                ImGui::TextWrapped(
                    "BiRefNet live preview: %s",
                    birefnetJob.livePreviewPath.c_str());
            }
            ImGui::SeparatorText("Artifact Inspection");
            const auto loadJobsTextPreview =
                [&](std::string label, std::string path) {
                    shellState->jobsTextPreviewLabel = std::move(label);
                    shellState->jobsTextPreviewPath = std::move(path);
                    shellState->jobsTextPreview = readTextFileTail(
                        fs::path(shellState->jobsTextPreviewPath),
                        64U * 1024U,
                        &shellState->jobsTextPreviewError);
                };
            const auto drawJobsArtifactButton =
                [&](const char* buttonLabel,
                    const char* previewLabel,
                    const std::string& path) {
                    ImGui::BeginDisabled(path.empty());
                    if (ImGui::Button(buttonLabel)) {
                        loadJobsTextPreview(previewLabel, path);
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                };
            drawJobsArtifactButton(
                "Face Manifest", "Face-detection manifest",
                faceJob.manifestPath);
            drawJobsArtifactButton(
                "Face Log", "Face-detection log", faceJob.logPath);
            drawJobsArtifactButton(
                "Proxy Manifest", "Proxy manifest",
                proxyJob.manifestPath);
            drawJobsArtifactButton(
                "Mask Manifest", "Prompt-mask manifest",
                promptMaskJob.manifestPath);
            drawJobsArtifactButton(
                "Transcript Manifest", "Transcription manifest",
                transcriptionJob.manifestPath);
            drawJobsArtifactButton(
                "Transcript Log", "Transcription log",
                transcriptionJob.logPath);
            drawJobsArtifactButton(
                "BiRefNet Manifest", "BiRefNet manifest",
                birefnetJob.manifestPath);
            drawJobsArtifactButton(
                "BiRefNet Log", "BiRefNet log",
                birefnetJob.logPath);
            drawJobsArtifactButton(
                "BiRefNet Progress", "BiRefNet progress",
                birefnetJob.progressPath);
            ImGui::BeginDisabled(promptMaskJob.logPath.empty());
            if (ImGui::Button("Mask Log")) {
                loadJobsTextPreview(
                    "Prompt-mask log", promptMaskJob.logPath);
            }
            ImGui::EndDisabled();
            if (!shellState->jobsTextPreviewPath.empty()) {
                ImGui::Separator();
                ImGui::Text(
                    "%s (%zu bytes shown)",
                    shellState->jobsTextPreviewLabel.c_str(),
                    shellState->jobsTextPreview.size());
                ImGui::TextWrapped(
                    "%s", shellState->jobsTextPreviewPath.c_str());
                if (!shellState->jobsTextPreviewError.empty()) {
                    ImGui::TextColored(
                        ImVec4(1.0f, 0.45f, 0.35f, 1.0f),
                        "%s",
                        shellState->jobsTextPreviewError.c_str());
                }
                if (ImGui::Button("Refresh Artifact")) {
                    loadJobsTextPreview(
                        shellState->jobsTextPreviewLabel.c_str(),
                        shellState->jobsTextPreviewPath);
                }
                ImGui::SameLine();
                if (ImGui::Button("Close Artifact")) {
                    shellState->jobsTextPreviewLabel.clear();
                    shellState->jobsTextPreviewPath.clear();
                    shellState->jobsTextPreview.clear();
                    shellState->jobsTextPreviewError.clear();
                }
                if (!shellState->jobsTextPreviewPath.empty()) {
                    ImGui::InputTextMultiline(
                        "##JobsArtifactText",
                        &shellState->jobsTextPreview,
                        ImVec2(-1.0f, 220.0f),
                        ImGuiInputTextFlags_ReadOnly);
                }
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("AI Assist")) {
            drawInspectorHeading("AI Assist", snapshot, currentClip);
            ImGui::TextWrapped(
                "%s",
                shellState->aiAccount.status.empty()
                    ? "Configure credentials and refresh account access."
                    : shellState->aiAccount.status.c_str());
            ImGui::Text(
                "Project usage %d/%d (failures %d)",
                shellState->aiUsageRequests,
                shellState->aiUsageBudgetCap,
                shellState->aiUsageFailures);
            std::vector<std::string> modelOptions =
                shellState->aiAccount.entitlements.models;
            if (modelOptions.empty()) {
                modelOptions = {
                    "deepseek-chat",
                    "gpt-4o-mini",
                    "mistral-small",
                    "qwen2.5-7b-instruct"};
            }
            if (ImGui::BeginCombo(
                    "Model", shellState->aiSelectedModel.c_str())) {
                for (const std::string& model : modelOptions) {
                    const bool selected =
                        model == shellState->aiSelectedModel;
                    if (ImGui::Selectable(model.c_str(), selected)) {
                        shellState->aiSelectedModel = model;
                        setLegacyStateOverride(
                            shellState,
                            "aiSelectedModel",
                            shellState->aiSelectedModel);
                    }
                }
                ImGui::EndCombo();
            }
            if (ImGui::BeginChild(
                    "AiChatHistory", ImVec2(-1.0f, 210.0f), true)) {
                for (const AiChatMessage& message :
                     shellState->aiChatMessages) {
                    ImGui::TextColored(
                        message.role == "Error"
                            ? ImVec4(1.0f, 0.45f, 0.35f, 1.0f)
                            : ImVec4(0.45f, 0.75f, 1.0f, 1.0f),
                        "%s",
                        message.role.c_str());
                    ImGui::TextWrapped("%s", message.content.c_str());
                    ImGui::Spacing();
                }
                if (shellState->aiTaskRunning) {
                    ImGui::TextDisabled("Waiting for AI response...");
                }
                if (ImGui::GetScrollY() >=
                    ImGui::GetScrollMaxY() - 4.0f) {
                    ImGui::SetScrollHereY(1.0f);
                }
            }
            ImGui::EndChild();
            const bool submitOnEnter = ImGui::InputTextMultiline(
                "##AiChatPrompt",
                &shellState->aiChatPrompt,
                ImVec2(-1.0f, 72.0f),
                ImGuiInputTextFlags_CtrlEnterForNewLine |
                    ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::BeginDisabled(
                shellState->aiTaskRunning ||
                shellState->aiChatPrompt.empty() ||
                !shellState->aiAccount.aiEnabled);
            if (ImGui::Button("Send") || submitOnEnter) {
                startAiChatRequest(shellState, snapshot);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(shellState->aiChatMessages.empty());
            if (ImGui::Button("Clear Chat")) {
                shellState->aiChatMessages.clear();
            }
            ImGui::EndDisabled();
            ImGui::Separator();
            ImGui::TextWrapped(
                "Deterministic speaker-name, organization, and spurious-label "
                "mining remains available in Speakers without cloud access.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Access")) {
            drawInspectorHeading("Subscriptions & Purchases", snapshot, currentClip);
            const jcut::ai::AccessTokenProfileCore tokenProfile =
                jcut::ai::parseAccessTokenProfileCore(
                    shellState->aiSessionToken);
            const std::string accountIdentity =
                !shellState->aiUserId.empty()
                    ? shellState->aiUserId
                    : !shellState->aiAccount.entitlements.userId.empty()
                        ? shellState->aiAccount.entitlements.userId
                        : tokenProfile.displayIdentity();
            if (!accountIdentity.empty()) {
                drawAiProfileAvatar(
                    *shellState, accountIdentity);
                ImGui::SameLine();
                ImGui::BeginGroup();
                ImGui::TextUnformatted(accountIdentity.c_str());
                const bool subscribed =
                    shellState->aiAccount.usage.hasSubscription;
                const bool entitled =
                    shellState->aiAccount.entitlements.entitled;
                ImGui::TextColored(
                    subscribed
                        ? ImVec4(0.95f, 0.76f, 0.24f, 1.0f)
                        : entitled
                            ? ImVec4(0.45f, 0.85f, 0.55f, 1.0f)
                            : ImVec4(0.65f, 0.69f, 0.74f, 1.0f),
                    "%s",
                    subscribed
                        ? "SUBSCRIBED"
                        : entitled ? "AI ENABLED" : "BASIC");
                if (shellState->aiAvatarRunning) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("Loading profile image...");
                }
                ImGui::EndGroup();
            } else {
                ImGui::TextDisabled("Not signed in");
            }
            if (ImGui::InputText(
                    "Gateway",
                    &shellState->aiGatewayBaseUrl,
                    ImGuiInputTextFlags_EnterReturnsTrue)) {
                shellState->aiGatewayBaseUrl =
                    jcut::ai::normalizeGatewayBaseUrl(
                        shellState->aiGatewayBaseUrl);
                setLegacyStateOverride(
                    shellState,
                    "aiProxyBaseUrl",
                    shellState->aiGatewayBaseUrl);
            }
            ImGui::InputText(
                "Session Token",
                &shellState->aiSessionToken,
                ImGuiInputTextFlags_Password);
            ImGui::TextDisabled(
                "Token is never stored in the project. "
                "JCUT_AI_AUTH_TOKEN is loaded at startup.");
            ImGui::BeginDisabled(
                shellState->aiBrowserLoginRunning ||
                !jcut::ai::isSupabaseGatewayBase(
                    shellState->aiGatewayBaseUrl));
            if (ImGui::Button("Log In with Browser")) {
                startAiBrowserLogin(shellState);
            }
            ImGui::EndDisabled();
            if (shellState->aiBrowserLoginRunning) {
                ImGui::SameLine();
                if (ImGui::Button("Cancel Login")) {
                    shellState->aiBrowserLoginCancelRequested.store(true);
                }
            }
            ImGui::BeginDisabled(shellState->aiSessionToken.empty());
            if (ImGui::Button("Save Login Securely")) {
                const jcut::ai::CredentialStoreResultCore stored =
                    jcut::ai::storeCredentialsCore(
                        jcut::ai::StoredCredentialsCore{
                            shellState->aiSessionToken,
                            shellState->aiRefreshToken,
                            shellState->aiUserId});
                shellState->aiCredentialStatus = stored.ok
                    ? (stored.usedSystemStore
                           ? "Login saved in the system secret store."
                           : "Login saved in the private config fallback.")
                    : stored.error;
                appendAiActivity(
                    shellState,
                    stored.ok ? "Credentials" : "Credential error",
                    shellState->aiCredentialStatus);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(
                shellState->aiRefreshToken.empty() ||
                shellState->aiTokenRefreshRunning);
            if (ImGui::Button("Refresh Login Token")) {
                startAiTokenRefresh(shellState);
            }
            ImGui::EndDisabled();
            if (!shellState->aiCredentialStatus.empty()) {
                ImGui::TextDisabled(
                    "%s", shellState->aiCredentialStatus.c_str());
            }
            ImGui::BeginDisabled(
                shellState->aiAccountRefreshRunning ||
                shellState->aiGatewayBaseUrl.empty() ||
                shellState->aiSessionToken.empty() ||
                !shellState->featureAiPanel);
            if (ImGui::Button("Refresh Access")) {
                startAiAccountRefresh(shellState);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(shellState->aiSessionToken.empty());
            if (ImGui::Button("Log Out")) {
                const jcut::ai::CredentialStoreResultCore cleared =
                    jcut::ai::clearStoredCredentialsCore();
                shellState->aiSessionToken.clear();
                shellState->aiRefreshToken.clear();
                shellState->aiUserId.clear();
                shellState->aiAccount = {};
                shellState->aiAccount.status =
                    "Session credentials cleared.";
                shellState->aiCredentialStatus = cleared.ok
                    ? "Stored login cleared."
                    : cleared.error;
                appendAiActivity(
                    shellState,
                    cleared.ok ? "Logout" : "Logout error",
                    shellState->aiCredentialStatus);
            }
            ImGui::EndDisabled();
            if (shellState->aiAccountRefreshRunning) {
                ImGui::SameLine();
                ImGui::TextDisabled("Refreshing...");
            }
            ImGui::BeginDisabled(
                shellState->aiCheckoutRunning ||
                shellState->aiSessionToken.empty() ||
                !shellState->featureAiPanel);
            if (ImGui::Button("Subscribe / Open Checkout")) {
                startAiCheckout(shellState);
            }
            ImGui::EndDisabled();
            if (shellState->aiCheckoutRunning) {
                ImGui::SameLine();
                ImGui::TextDisabled("Opening checkout...");
            }
            ImGui::TextWrapped(
                "%s",
                shellState->aiAccount.status.empty()
                    ? "No account data loaded."
                    : shellState->aiAccount.status.c_str());
            if (ImGui::BeginTable("AccessTable", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Type");
                ImGui::TableSetupColumn("Item");
                ImGui::TableSetupColumn("Status");
                ImGui::TableSetupColumn("Period");
                ImGui::TableSetupColumn("Source");
                ImGui::TableHeadersRow();
                for (const jcut::ai::AccessRowCore& row :
                     shellState->aiAccount.rows) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(row.type.c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(row.item.c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(row.status.c_str());
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(row.period.c_str());
                    ImGui::TableSetColumnIndex(4);
                    ImGui::TextUnformatted(row.source.c_str());
                }
                ImGui::EndTable();
            }
            ImGui::SeparatorText("AI Activity");
            ImGui::SameLine();
            ImGui::BeginDisabled(
                shellState->aiActivityEntries.empty());
            if (ImGui::SmallButton("Clear Activity")) {
                shellState->aiActivityEntries.clear();
            }
            ImGui::EndDisabled();
            if (ImGui::BeginTable(
                    "AiActivityTable",
                    3,
                    ImGuiTableFlags_Borders |
                        ImGuiTableFlags_RowBg |
                        ImGuiTableFlags_ScrollY,
                    ImVec2(-1.0f, 180.0f))) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn(
                    "Time",
                    ImGuiTableColumnFlags_WidthFixed,
                    72.0f);
                ImGui::TableSetupColumn(
                    "Phase",
                    ImGuiTableColumnFlags_WidthFixed,
                    110.0f);
                ImGui::TableSetupColumn(
                    "Summary",
                    ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();
                for (const AiActivityEntry& entry :
                     shellState->aiActivityEntries) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(entry.time.c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(entry.phase.c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextWrapped(
                        "%s", entry.summary.c_str());
                }
                ImGui::EndTable();
            }
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
        if (ImGui::BeginTabItem(
                "Clip", nullptr, inspectorTabFlags("Clip"))) {
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
            std::size_t exportQueueCurrent = 0;
            std::size_t exportQueueTotal = 0;
            std::size_t exportQueueCompleted = 0;
            std::size_t exportQueueFailed = 0;
            std::string exportQueueLabel;
            {
                std::lock_guard<std::mutex> lock(shellState->exportMutex);
                exportProgress = shellState->exportProgress;
                exportResult = shellState->exportResult;
                exportRunning = shellState->exportRunning;
                exportHasProgress = shellState->exportHasProgress;
                exportQueueCurrent =
                    shellState->exportQueueCurrent;
                exportQueueTotal =
                    shellState->exportQueueTotal;
                exportQueueCompleted =
                    shellState->exportQueueCompleted;
                exportQueueFailed =
                    shellState->exportQueueFailed;
                exportQueueLabel =
                    shellState->exportQueueLabel;
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
                if (exportQueueTotal > 1) {
                    ImGui::Text(
                        "Batch %zu / %zu | completed %zu | failed %zu",
                        exportQueueCurrent,
                        exportQueueTotal,
                        exportQueueCompleted,
                        exportQueueFailed);
                    if (!exportQueueLabel.empty()) {
                        ImGui::TextWrapped(
                            "%s",
                            exportQueueLabel.c_str());
                    }
                }
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
            const std::vector<PipelineStageCore> stages =
                previewPipelineStages(
                    previewResult,
                    lastUsedZeroCopy,
                    zeroCopyAvailable,
                    zeroCopyFailure);
            shellState->selectedPipelineStage = std::clamp(
                shellState->selectedPipelineStage,
                0,
                std::max(0, static_cast<int>(stages.size()) - 1));
            if (ImGui::Button("Refresh Pipeline")) {
                requestPreviewRender(shellState);
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(lastUsedZeroCopy);
            if (ImGui::Button("Retry Zero Copy")) {
                {
                    std::lock_guard<std::mutex> lock(
                        shellState->previewMutex);
                    shellState->previewCpuFallbackPreferred = false;
                    shellState->previewZeroCopyFailureReason.clear();
                }
                requestPreviewRender(shellState);
            }
            ImGui::EndDisabled();
            if (ImGui::BeginChild(
                    "PipelineGraph",
                    ImVec2(0.0f, 104.0f),
                    true,
                    ImGuiWindowFlags_HorizontalScrollbar)) {
                for (std::size_t index = 0;
                     index < stages.size();
                     ++index) {
                    const PipelineStageCore& stage = stages[index];
                    ImGui::PushID(static_cast<int>(index));
                    const ImVec4 color =
                        stage.state == "blocked"
                        ? ImVec4(0.42f, 0.16f, 0.16f, 1.0f)
                        : (stage.state == "fallback"
                               ? ImVec4(0.46f, 0.34f, 0.13f, 1.0f)
                               : (stage.active
                                      ? ImVec4(0.15f, 0.42f, 0.31f, 1.0f)
                                      : ImVec4(0.19f, 0.23f, 0.28f, 1.0f)));
                    ImGui::PushStyleColor(
                        ImGuiCol_Button, color);
                    ImGui::PushStyleColor(
                        ImGuiCol_ButtonHovered,
                        ImVec4(
                            std::min(1.0f, color.x + 0.12f),
                            std::min(1.0f, color.y + 0.12f),
                            std::min(1.0f, color.z + 0.12f),
                            1.0f));
                    const std::string cardLabel =
                        stage.label + "\n" + stage.state;
                    if (ImGui::Button(
                            cardLabel.c_str(),
                            ImVec2(112.0f, 68.0f))) {
                        shellState->selectedPipelineStage =
                            static_cast<int>(index);
                    }
                    ImGui::PopStyleColor(2);
                    if (index + 1 < stages.size()) {
                        ImGui::SameLine();
                        ImGui::TextUnformatted(">");
                        ImGui::SameLine();
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();
            if (!stages.empty()) {
                const PipelineStageCore& selectedStage =
                    stages[static_cast<std::size_t>(
                        shellState->selectedPipelineStage)];
                ImGui::SeparatorText(selectedStage.label.c_str());
                ImGui::Text(
                    "%s • %s • %s",
                    selectedStage.kind.c_str(),
                    selectedStage.state.c_str(),
                    selectedStage.exact ? "exact" : "approximate");
                ImGui::TextWrapped(
                    "%s", selectedStage.detail.c_str());
                if (ImGui::BeginTable(
                        "PipelineStageFacts",
                        2,
                        ImGuiTableFlags_Borders |
                            ImGuiTableFlags_RowBg)) {
                    for (const auto& [label, value] :
                         selectedStage.facts) {
                        drawReadOnlyTableRow(label.c_str(), value);
                    }
                    ImGui::EndTable();
                }
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("System")) {
            drawInspectorHeading("System", snapshot, currentClip);
            const jcut::ImGuiAudioStatus audioStatus = shellState->audioRuntime.status();
            jcut::standalone_render::PreviewRenderResult decoderPreview;
            {
                std::lock_guard<std::mutex> lock(
                    shellState->previewMutex);
                decoderPreview = shellState->previewResult;
            }
            const jcut::EffectiveDecoderPolicyCore effectiveDecoder =
                jcut::effectiveDecoderPolicyCore(
                    shellState->decoderPolicy,
                    decoderPreview.hardwareAccelerated,
                    false,
                    static_cast<int>(std::max(
                        1U, std::thread::hardware_concurrency())));
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
                drawReadOnlyTableRow(
                    "Audio Device Requested",
                    audioStatus.requestedOutputDeviceName.empty()
                    ? std::string("system default")
                    : audioStatus.requestedOutputDeviceName);
                drawReadOnlyTableRow(
                    "Audio Device Active",
                    audioStatus.activeOutputDeviceName.empty()
                    ? std::string("stream closed")
                    : audioStatus.activeOutputDeviceName);
                drawReadOnlyTableRow(
                    "Decode Requested",
                    jcut::decodePreferenceCoreName(
                        shellState->decoderPolicy.decodePreference));
                drawReadOnlyTableRow(
                    "Decode Effective",
                    jcut::decodePreferenceCoreName(
                        decoderPreview.sourcePath.empty()
                        ? effectiveDecoder.effectivePreference
                        : decoderPreview.effectiveDecodePreference));
                drawReadOnlyTableRow(
                    "Decode Device",
                    decoderPreview.hardwareDeviceLabel.empty()
                    ? std::string("software")
                    : decoderPreview.hardwareDeviceLabel);
                drawReadOnlyTableRow(
                    "H.26x Threads",
                    std::to_string(
                        effectiveDecoder.softwareThreadCount));
                drawReadOnlyTableRow(
                    "Deterministic",
                    shellState->decoderPolicy.deterministic
                    ? "yes" : "no");
                ImGui::EndTable();
            }
            ImGui::SeparatorText("Decoder Policy");
            const std::array<const char*, 4> decodeLabels{
                "Auto", "Hardware Zero-Copy", "Hardware", "Software"};
            int decodeIndex = static_cast<int>(
                shellState->decoderPolicy.decodePreference);
            if (ImGui::Combo(
                    "Decode Preference",
                    &decodeIndex,
                    decodeLabels.data(),
                    static_cast<int>(decodeLabels.size()))) {
                shellState->decoderPolicy.decodePreference =
                    static_cast<jcut::DecodePreferenceCore>(
                        std::clamp(decodeIndex, 0, 3));
                saveUiPreferences(*shellState);
                requestPreviewRender(shellState);
            }
            const std::array<const char*, 6> hardwareDeviceLabels{
                "Auto",
                "CUDA (NVIDIA)",
                "VA-API",
                "VideoToolbox",
                "D3D11VA",
                "DXVA2"};
            int hardwareDeviceIndex = static_cast<int>(
                shellState->decoderPolicy.hardwareDevice);
            if (ImGui::Combo(
                    "Hardware Decode Device",
                    &hardwareDeviceIndex,
                    hardwareDeviceLabels.data(),
                    static_cast<int>(
                        hardwareDeviceLabels.size()))) {
                shellState->decoderPolicy.hardwareDevice =
                    static_cast<jcut::DecodeHardwareDeviceCore>(
                        std::clamp(hardwareDeviceIndex, 0, 5));
                saveUiPreferences(*shellState);
                requestPreviewRender(shellState);
            }
            const std::array<const char*, 4> threadingLabels{
                "Auto (Stability)",
                "Single Thread",
                "Slice Threads",
                "Frame + Slice Threads"};
            int threadingIndex = static_cast<int>(
                shellState->decoderPolicy.h26xThreadingMode);
            if (ImGui::Combo(
                    "H.264/H.265 CPU Threading",
                    &threadingIndex,
                    threadingLabels.data(),
                    static_cast<int>(threadingLabels.size()))) {
                shellState->decoderPolicy.h26xThreadingMode =
                    static_cast<jcut::H26xThreadingModeCore>(
                        std::clamp(threadingIndex, 0, 3));
                setLegacyStateOverride(
                    shellState,
                    "debugH26xSoftwareThreadingMode",
                    jcut::h26xThreadingModeCoreName(
                        shellState->decoderPolicy.h26xThreadingMode));
                requestPreviewRender(shellState);
            }
            bool deterministic =
                shellState->decoderPolicy.deterministic;
            if (ImGui::Checkbox(
                    "Deterministic Pipeline", &deterministic)) {
                shellState->decoderPolicy.deterministic = deterministic;
                setLegacyStateOverride(
                    shellState,
                    "debugDeterministicPipeline",
                    deterministic);
                setLegacyStateOverride(
                    shellState,
                    "debugDeterministicPipelineExplicit",
                    true);
                requestPreviewRender(shellState);
            }
            int decoderLanes =
                shellState->decoderPolicy.decoderLaneCount;
            if (ImGui::InputInt("Decoder Lane Count", &decoderLanes)) {
                shellState->decoderPolicy.decoderLaneCount =
                    std::clamp(decoderLanes, 0, 16);
                setLegacyStateOverride(
                    shellState,
                    "debugDecoderLaneCount",
                    shellState->decoderPolicy.decoderLaneCount);
            }
            if (!decoderPreview.hardwareFallbackReason.empty()) {
                ImGui::TextDisabled(
                    "Hardware fallback: %s",
                    decoderPreview.hardwareFallbackReason.c_str());
            }
            if (ImGui::Button("Restart Preview Decoder")) {
                requestPreviewRender(shellState);
                shellState->statusMessage =
                    "standalone preview decoder restarted";
            }
            if (shellState->decodeBenchmarkRunning &&
                shellState->decodeBenchmarkFuture.valid() &&
                shellState->decodeBenchmarkFuture.wait_for(
                    std::chrono::seconds(0)) ==
                    std::future_status::ready) {
                shellState->decodeBenchmarkResult =
                    shellState->decodeBenchmarkFuture.get();
                shellState->decodeBenchmarkRunning = false;
                shellState->statusMessage =
                    shellState->decodeBenchmarkResult.message;
            }
            const bool benchmarkSourceAvailable =
                currentClip && currentClip->mediaKind != "title" &&
                !currentClip->sourcePath.empty();
            ImGui::SameLine();
            ImGui::BeginDisabled(
                shellState->decodeBenchmarkRunning ||
                !benchmarkSourceAvailable);
            if (ImGui::Button("Run Decode Benchmark") && currentClip) {
                const std::string benchmarkPath = pathString(
                    resolvedClipMediaPathForProbe(
                        *shellState, *currentClip));
                const jcut::DecoderPolicySettingsCore benchmarkPolicy =
                    shellState->decoderPolicy;
                shellState->decodeBenchmarkRunning = true;
                shellState->decodeBenchmarkFuture = std::async(
                    std::launch::async,
                    [benchmarkPath, benchmarkPolicy]() {
                        return jcut::standalone_render::
                            benchmarkStandaloneMediaDecode(
                                benchmarkPath, benchmarkPolicy);
                    });
            }
            ImGui::EndDisabled();
            if (shellState->decodeBenchmarkRunning) {
                ImGui::TextUnformatted("Decode benchmark running...");
            } else if (!shellState->decodeBenchmarkResult.message.empty()) {
                const auto& benchmark =
                    shellState->decodeBenchmarkResult;
                ImGui::Text(
                    "%s | %d frames | %d failed | %.1f fps | %lld ms",
                    benchmark.codecName.empty()
                    ? "unknown codec"
                    : benchmark.codecName.c_str(),
                    benchmark.framesDecoded,
                    benchmark.failedFrames,
                    benchmark.framesPerSecond,
                    static_cast<long long>(benchmark.elapsedMs));
                if (benchmark.hardwareAccelerated) {
                    ImGui::Text(
                        "Hardware decode active: %s",
                        benchmark.hardwareDeviceLabel.empty()
                        ? "device"
                        : benchmark.hardwareDeviceLabel.c_str());
                } else if (!benchmark.hardwareFallbackReason.empty()) {
                    ImGui::TextDisabled(
                        "Hardware fallback: %s",
                        benchmark.hardwareFallbackReason.c_str());
                }
            }
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
            const jcut::ImGuiAudioStatus audioStatus =
                shellState->audioRuntime.status();
            const std::string selectedDeviceLabel =
                shellState->audioOutputDeviceName.empty()
                ? "System Default"
                : shellState->audioOutputDeviceName;
            if (ImGui::BeginCombo(
                    "Audio Output Device",
                    selectedDeviceLabel.c_str())) {
                const bool defaultSelected =
                    shellState->audioOutputDeviceName.empty();
                if (ImGui::Selectable(
                        "System Default", defaultSelected)) {
                    shellState->audioOutputDeviceName.clear();
                    shellState->audioRuntime.setOutputDeviceName({});
                    saveUiPreferences(*shellState);
                    shellState->statusMessage =
                        "default audio output selected";
                }
                for (const jcut::ImGuiAudioOutputDevice& device :
                     audioStatus.outputDevices) {
                    const bool selected =
                        device.name == shellState->audioOutputDeviceName;
                    const std::string label = device.name +
                        (device.isDefault ? " (default)" : "");
                    if (ImGui::Selectable(label.c_str(), selected)) {
                        shellState->audioOutputDeviceName = device.name;
                        shellState->audioRuntime.setOutputDeviceName(
                            device.name);
                        saveUiPreferences(*shellState);
                        shellState->statusMessage =
                            "audio output updated; playback will restart";
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::Button("Refresh Audio Devices")) {
                shellState->audioRuntime.refreshOutputDevices();
                shellState->statusMessage =
                    "audio output devices refreshed";
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
            ImGui::SeparatorText("AI Access");
            if (ImGui::Checkbox(
                    "Enable AI Panel",
                    &shellState->featureAiPanel)) {
                setLegacyStateOverride(
                    shellState,
                    "feature_ai_panel",
                    shellState->featureAiPanel);
            }
            if (ImGui::Checkbox(
                    "Enable AI Speaker Cleanup",
                    &shellState->featureAiSpeakerCleanup)) {
                setLegacyStateOverride(
                    shellState,
                    "feature_ai_speaker_cleanup",
                    shellState->featureAiSpeakerCleanup);
            }
            int aiBudget = shellState->aiUsageBudgetCap;
            if (ImGui::InputInt(
                    "AI Project Budget", &aiBudget)) {
                shellState->aiUsageBudgetCap =
                    std::clamp(aiBudget, 1, 1000000);
                setLegacyStateOverride(
                    shellState,
                    "aiUsageBudgetCap",
                    shellState->aiUsageBudgetCap);
            }
            ImGui::TextDisabled(
                "Gateway, model, feature flags, and counters are project "
                "settings. Bearer tokens are never written to the project.");
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
    shellState.audioRuntime.setOutputDeviceName(
        shellState.audioOutputDeviceName);
    shellState.audioRuntime.refreshOutputDevices();
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
