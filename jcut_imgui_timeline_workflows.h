#pragma once

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
    ++shellState->legacyStateGeneration;
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
    ++shellState->legacyStateGeneration;
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
    const bool previewUsingGpu =
        previewResult.vulkanFrame.valid &&
        previewLastUsedZeroCopy;
    const bool anyGpuRendering =
        previewUsingGpu || exportResult.usedGpu;

    nlohmann::json renderStatus{
        {"ok", true},
        {"backend", shellState->gpuRenderer.available()
            ? "qt_free_vulkan"
            : "standalone_fallback"},
        {"sharedGpuRenderer", {
            {"available", shellState->gpuRenderer.available()},
            {"status", shellState->gpuRenderer.status()}
        }},
        {"path", previewUsingGpu
            ? "gpu_preview"
            : (exportResult.usedGpu ? "gpu_export" : "cpu_fallback")},
        {"usingGpu", anyGpuRendering},
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
    ++shellState->dirtyGeneration;
    ++shellState->legacyStateGeneration;
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
        ++shellState->dirtyGeneration;
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
    ++shellState->dirtyGeneration;
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
    ++shellState->dirtyGeneration;
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
    if (shellState.dirtyCacheGeneration ==
            shellState.dirtyGeneration &&
        shellState.dirtyCacheLegacyGeneration ==
            shellState.legacyStateGeneration) {
        return shellState.dirtyCacheValue;
    }
    shellState.dirtyCacheValue =
        snapshotJson(snapshot) != shellState.lastSavedSnapshotJson ||
        (shellState.usesQtProjectStorage &&
         legacyExtensionSignature(shellState) !=
             shellState.lastSavedLegacyExtensionSignature);
    shellState.dirtyCacheGeneration =
        shellState.dirtyGeneration;
    shellState.dirtyCacheLegacyGeneration =
        shellState.legacyStateGeneration;
    return shellState.dirtyCacheValue;
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
    ++shellState->dirtyGeneration;
    ++shellState->legacyStateGeneration;
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
            preferVulkanFrame =
                !shellState->previewCpuFallbackPreferred;
            shellState->previewRenderRequested = false;
        }

        const jcut::core::SizeI outputSize =
            document.exportRequest.outputSize.valid()
            ? document.exportRequest.outputSize
            : jcut::core::SizeI{1080, 1920};
        jcut::standalone_render::PreviewRenderResult result;
        std::string sharedGpuError;
        if (preferVulkanFrame &&
            shellState->gpuRenderer.renderPreview(
                document,
                rootDirectory,
                outputSize,
                document.transport.currentFrame,
                document.panels.showScopes,
                &result.vulkanFrame,
                &result.image,
                &sharedGpuError)) {
            result.success = true;
            result.message =
                "Qt-free Vulkan composition completed";
            result.hardwareDirectEligible = false;
            result.hardwareDeviceLabel =
                "Qt-free neutral Vulkan renderer";
            result.hardwareDirectFallbackReason =
                "decoded/effect layers used neutral CPU preparation before Vulkan composition";
        } else {
            result =
                jcut::standalone_render::renderPreviewFrame(
                    jcut::standalone_render::PreviewRenderRequest{
                        document,
                        outputSize,
                        document.transport.currentFrame,
                        rootDirectory,
                        false,
                        true,
                        decoderPolicy});
            if (!sharedGpuError.empty()) {
                result.hardwareDirectFallbackReason =
                    "shared GPU renderer: " +
                    sharedGpuError;
                result.message =
                    "CPU preview fallback: " +
                    sharedGpuError;
            }
        }

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
                shellState->gpuRenderer.exportTimeline(
                    queue[index].document,
                    rootDirectory,
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
            summary.exportPipeline = result.exportPipeline;
            summary.gpuTransferLabel =
                result.gpuTransferLabel;
            summary.encoderPixelFormat =
                result.encoderPixelFormat;
            summary.encoderSoftwarePixelFormat =
                result.encoderSoftwarePixelFormat;
            summary.cudaExternalMemoryStatus =
                result.cudaExternalMemoryStatus;
            summary.exportPathFallbackReason =
                result.exportPathFallbackReason;
            summary.cudaExternalTransfer =
                result.cudaExternalTransfer;
            summary.cudaExternalMemorySupported =
                result.cudaExternalMemorySupported;
            summary.encoderHardwareFrames =
                result.encoderHardwareFrames;
            summary.effectiveRenderBackend =
                result.effectiveRenderBackend;
            summary.requestedRenderBackend =
                result.requestedRenderBackend;
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

int frameFromTimelineX(
    float contentLeft,
    float mouseX,
    std::int64_t frameOffset,
    float pixelsPerFrame)
{
    return static_cast<int>(std::min<std::int64_t>(
        jcut::timeline_viewport::frameFromX(
            contentLeft,
            mouseX,
            frameOffset,
            pixelsPerFrame),
        std::numeric_limits<int>::max()));
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

int timelineSnapThresholdFrames(float pixelsPerFrame)
{
    return std::max(1, static_cast<int>(std::lround(
        kTimelineSnapThresholdPixels /
        std::max(0.25f, pixelsPerFrame))));
}

TimelineSnapResult snapTimelineBoundary(const jcut::EditorDocumentCore& snapshot,
                                        int proposedFrame,
                                        float pixelsPerFrame,
                                        int excludedClipId = 0,
                                        bool excludeSelected = false)
{
    const std::int64_t proposed = std::max(0, proposedFrame);
    const std::int64_t threshold =
        timelineSnapThresholdFrames(pixelsPerFrame);
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
                                         int proposedStartFrame,
                                         float pixelsPerFrame)
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
            timelineSnapThresholdFrames(pixelsPerFrame));
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

