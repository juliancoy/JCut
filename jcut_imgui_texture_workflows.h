#pragma once

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

