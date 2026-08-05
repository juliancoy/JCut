#pragma once

std::string validateExportRequest(
    const jcut::EditorDocumentCore& document)
{
    if (document.tracks.empty() || document.clips.empty()) {
        return "export requires at least one track and clip";
    }
    if (!document.exportRequest.outputSize.valid()) {
        return "export output size is invalid";
    }
    if (document.exportRequest.outputPath.empty()) {
        return "export output path is empty";
    }
    if (document.exportRequest.outputFormat.empty()) {
        return "export format is empty";
    }
    if (document.exportRequest.exportEndFrame <
        document.exportRequest.exportStartFrame) {
        return "export range end is before export range start";
    }
    return {};
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
        shellState->statusMessage =
            "could not commit export output path";
        return false;
    }
    jcut::EditorDocumentCore snapshot;
    {
        std::lock_guard<std::mutex> runtimeLock(shellState->runtimeMutex);
        snapshot = shellState->runtime.snapshot();
    }
    const std::string validationError =
        validateExportRequest(snapshot);
    if (!validationError.empty()) {
        shellState->statusMessage = validationError;
        return false;
    }
    std::lock_guard<std::mutex> lock(shellState->exportMutex);
    if (shellState->exportRunning || shellState->exportRequested) {
        shellState->statusMessage = "export already running";
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
    if (result.applied) {
        ++shellState->dirtyGeneration;
    }
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
