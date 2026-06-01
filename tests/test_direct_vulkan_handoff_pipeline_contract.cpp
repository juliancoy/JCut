#include <QtTest/QtTest>

#include <QFile>
#include <QString>

class TestDirectVulkanHandoffPipelineContract : public QObject {
    Q_OBJECT

private slots:
    void directPreviewUsesExtractedPipelineBeforeRenderPass();
    void directPreviewRecordsTextureUploadsBeforeRenderPass();
    void directPreviewDoesNotUseSubmitBasedHandoffApis();
    void directPreviewUsesPerClipHandoffDescriptors();
    void directPreviewRequiresHardwarePayloadsFromCache();
    void handoffPipelineRejectsCpuOnlyFrames();
    void strictDisplayabilityDoesNotAcceptCpuFallback();
    void directPreviewDisablesCpuAndQtTextOverlayFallbacks();
    void visibleDecodePriorityUsesTimelineDomain();
    void schedulingDiagnosticsExposeRequiredFields();
    void pipelineDiagnosticsDefaultToCompactSnapshot();
    void pitchPreservingAudioUsesExplicitSidecarGate();
    void overlayWorkerKeepsNewestCoalescedRequest();
    void rendererConsumesLatchedPreviewSnapshot();
    void vulkanTextShaderUsesVulkanFramebufferYConvention();
};

namespace {

QString readSourceFile(const QString& relativePath)
{
    QFile file(QStringLiteral(JCUT_SOURCE_DIR) + QLatin1Char('/') + relativePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

}

void TestDirectVulkanHandoffPipelineContract::directPreviewUsesExtractedPipelineBeforeRenderPass()
{
    const QString source = readSourceFile(QStringLiteral("direct_vulkan_preview_backend.cpp"));
    QVERIFY2(!source.isEmpty(), "direct_vulkan_preview_backend.cpp must be readable");

    const qsizetype recordIndex = source.indexOf(QStringLiteral("handoffResources->pipeline->record("));
    const qsizetype beginRenderPassIndex = source.indexOf(QStringLiteral("vkCmdBeginRenderPass"));
    QVERIFY2(recordIndex >= 0, "direct preview must call the extracted frame handoff pipeline");
    QVERIFY2(beginRenderPassIndex >= 0, "direct preview must explicitly begin its render pass");
    QVERIFY2(recordIndex < beginRenderPassIndex,
             "handoff transfer/compute recording must happen before vkCmdBeginRenderPass");
}

void TestDirectVulkanHandoffPipelineContract::directPreviewRecordsTextureUploadsBeforeRenderPass()
{
    const QString source = readSourceFile(QStringLiteral("direct_vulkan_preview_backend.cpp"));
    QVERIFY2(!source.isEmpty(), "direct_vulkan_preview_backend.cpp must be readable");

    const qsizetype beginRenderPassIndex = source.indexOf(QStringLiteral("vkCmdBeginRenderPass"));
    QVERIFY2(beginRenderPassIndex >= 0, "direct preview must explicitly begin its render pass");

    const QStringList uploadMarkers{
        QStringLiteral("uploadCurveLut(cb,"),
        QStringLiteral("uploadImageTexture(cb, overlayImage)")
    };
    for (const QString& marker : uploadMarkers) {
        qsizetype index = source.indexOf(marker);
        QVERIFY2(index >= 0, qPrintable(QStringLiteral("direct preview must contain %1").arg(marker)));
        while (index >= 0) {
            QVERIFY2(index < beginRenderPassIndex,
                     qPrintable(QStringLiteral("%1 must be recorded before vkCmdBeginRenderPass").arg(marker)));
            index = source.indexOf(marker, index + marker.size());
        }
    }
}

void TestDirectVulkanHandoffPipelineContract::directPreviewDoesNotUseSubmitBasedHandoffApis()
{
    const QString source = readSourceFile(QStringLiteral("direct_vulkan_preview_backend.cpp"));
    QVERIFY2(!source.isEmpty(), "direct_vulkan_preview_backend.cpp must be readable");

    QVERIFY2(!source.contains(QStringLiteral("m_frameHandoff->uploadFrame(")),
             "direct preview must not call the submit-based uploadFrame API");
    QVERIFY2(!source.contains(QStringLiteral("m_frameHandoff->importOffscreenFrame(")),
             "direct preview must not call the submit-based importOffscreenFrame API");
    QVERIFY2(!source.contains(QStringLiteral("uploadImageTexture(cb, status->frame.cpuImage()")),
             "direct preview must not implicitly fall back to CPU frame upload");
}

void TestDirectVulkanHandoffPipelineContract::directPreviewUsesPerClipHandoffDescriptors()
{
    const QString backend = readSourceFile(QStringLiteral("direct_vulkan_preview_backend.cpp"));
    QVERIFY2(!backend.isEmpty(), "direct_vulkan_preview_backend.cpp must be readable");

    QVERIFY2(backend.contains(QStringLiteral("QHash<QString, std::shared_ptr<ClipHandoffResources>> m_clipHandoffResources")),
             "direct preview must own separate handoff resources per active clip");
    QVERIFY2(backend.contains(QStringLiteral("QVector<RetiredClipHandoffResources> m_retiredClipHandoffResources")),
             "inactive handoff resources must be retired briefly instead of destroyed while swapchain frames may still be in flight");
    QVERIFY2(backend.contains(QStringLiteral("ensureClipHandoffResources(status.clipId)")),
             "direct preview must resolve handoff resources by clip id");
    QVERIFY2(backend.contains(QStringLiteral("pruneClipHandoffResources(activeHandoffClipIds)")),
             "direct preview must release per-clip handoff resources when clips leave the active render set");
    QVERIFY2(backend.contains(QStringLiteral("static_cast<int>(VulkanResources::kDescriptorSetCount) + 1")),
             "retired handoff resources must stay alive for at least the descriptor ring depth");
    QVERIFY2(backend.contains(QStringLiteral("handoffResult.descriptorSet")),
             "clip draws must bind the descriptor set captured by that clip's handoff result");
    QVERIFY2(backend.contains(QStringLiteral("activeClipHandoffResourceCount")),
             "direct preview diagnostics must expose active per-clip handoff resource ownership");
    QVERIFY2(backend.contains(QStringLiteral("retiredClipHandoffResourceCount")),
             "direct preview diagnostics must expose retired in-flight handoff resource ownership");
    QVERIFY2(!backend.contains(QStringLiteral("multi_clip_handoff_requires_descriptor_pool")),
             "multiple active clips must not be rejected due to a shared sampled-image descriptor");

    const QString surface = readSourceFile(QStringLiteral("vulkan_preview_surface.cpp"));
    QVERIFY2(!surface.isEmpty(), "vulkan_preview_surface.cpp must be readable");
    QVERIFY2(surface.contains(QStringLiteral("active_clip_handoff_resource_count")) &&
                 surface.contains(QStringLiteral("retired_clip_handoff_resource_count")),
             "stage 11 diagnostics must include active and retired handoff resource ownership");

    const QString presenter = readSourceFile(QStringLiteral("direct_vulkan_preview_presenter.cpp"));
    QVERIFY2(!presenter.isEmpty(), "direct_vulkan_preview_presenter.cpp must be readable");
    QVERIFY2(presenter.contains(QStringLiteral("active_clip_handoff_resource_count")),
             "presenter diagnostics must expose active per-clip handoff resource count");
    QVERIFY2(presenter.contains(QStringLiteral("retired_clip_handoff_resource_count")),
             "presenter diagnostics must expose retired per-clip handoff resource count");
    QVERIFY2(presenter.contains(QStringLiteral("explicit_failure_draw_count")),
             "compact presenter diagnostics must expose explicit failure draws");

    const QString pipeline = readSourceFile(QStringLiteral("direct_vulkan_frame_handoff_pipeline.h"));
    QVERIFY2(!pipeline.isEmpty(), "direct_vulkan_frame_handoff_pipeline.h must be readable");
    QVERIFY2(pipeline.contains(QStringLiteral("VkDescriptorSet descriptorSet")),
             "handoff result must carry the descriptor set whose sampled image it updated");

    const QString resources = readSourceFile(QStringLiteral("vulkan_resources.h"));
    QVERIFY2(resources.contains(QStringLiteral("static constexpr size_t kDescriptorSetCount")),
             "descriptor ring depth must be visible to the presenter lifetime policy");
}

void TestDirectVulkanHandoffPipelineContract::directPreviewRequiresHardwarePayloadsFromCache()
{
    const QString source = readSourceFile(QStringLiteral("vulkan_preview_surface.cpp"));
    QVERIFY2(!source.isEmpty(), "vulkan_preview_surface.cpp must be readable");

    QVERIFY2(source.contains(QStringLiteral("constexpr bool requireDirectVulkanPayload = true;")),
             "direct Vulkan preview must request strict hardware/GPU payloads");
    QVERIFY2(source.contains(QStringLiteral("requireDirectVulkanPayload);")),
             "strict payload requirement must be passed into visible frame requests");
}

void TestDirectVulkanHandoffPipelineContract::handoffPipelineRejectsCpuOnlyFrames()
{
    const QString source = readSourceFile(QStringLiteral("direct_vulkan_frame_handoff_pipeline.cpp"));
    QVERIFY2(!source.isEmpty(), "direct_vulkan_frame_handoff_pipeline.cpp must be readable");

    QVERIFY2(source.contains(QStringLiteral("!status.externalVulkanFrame && !status.frame.hasHardwareFrame()")),
             "handoff pipeline must reject frames that are not external Vulkan or hardware frames");
    QVERIFY2(source.contains(QStringLiteral("CPU upload fallback is disabled")),
             "handoff pipeline must report CPU fallback rejection explicitly");
    QVERIFY2(!source.contains(QStringLiteral("status.frame.cpuImage()")),
             "handoff pipeline must not consume CPU image payloads");
}

void TestDirectVulkanHandoffPipelineContract::strictDisplayabilityDoesNotAcceptCpuFallback()
{
    const QString source = readSourceFile(QStringLiteral("timeline_cache_requests.cpp"));
    QVERIFY2(!source.isEmpty(), "timeline_cache_requests.cpp must be readable");

    QVERIFY2(source.contains(QStringLiteral("requireHardwareOrGpuPayload &&")),
             "timeline cache must distinguish strict hardware/GPU preview requests");
    QVERIFY2(source.contains(QStringLiteral("!frame.hasHardwareFrame() && !frame.hasGpuTexture()")),
             "strict visible preview must reject non-hardware/non-GPU payloads");
    QVERIFY2(source.contains(QStringLiteral("strictPayloadRejected")),
             "strict CPU-payload rejection must be counted for diagnostics");
}

void TestDirectVulkanHandoffPipelineContract::directPreviewDisablesCpuAndQtTextOverlayFallbacks()
{
    const QString backend = readSourceFile(QStringLiteral("direct_vulkan_preview_backend.cpp"));
    QVERIFY2(!backend.isEmpty(), "direct_vulkan_preview_backend.cpp must be readable");
    QVERIFY2(backend.contains(QStringLiteral("kAllowCpuRasterTextOverlaysInDirectVulkanPreview = false")),
             "direct Vulkan preview must not CPU-rasterize title/transcript/speaker/status text overlays");

    const QString presenter = readSourceFile(QStringLiteral("direct_vulkan_preview_presenter.cpp"));
    QVERIFY2(!presenter.isEmpty(), "direct_vulkan_preview_presenter.cpp must be readable");
    QVERIFY2(presenter.contains(QStringLiteral("kAllowQtPainterOverlayInDirectVulkanPreview = false")),
             "direct Vulkan preview must not paint presentation overlays through Qt/QPainter");

    const QString profiling = readSourceFile(QStringLiteral("vulkan_preview_surface_profiling.cpp"));
    QVERIFY2(!profiling.isEmpty(), "vulkan_preview_surface_profiling.cpp must be readable");
    QVERIFY2(profiling.contains(QStringLiteral("vulkan_text_overlay_cpu_rasterization_enabled")),
             "profiling must expose the CPU text overlay contract");
    QVERIFY2(profiling.contains(QStringLiteral("vulkan_text_overlay_qt_painter_enabled")),
             "profiling must expose the Qt painter overlay contract");
    QVERIFY2(profiling.contains(QStringLiteral("vulkan_speaker_label_gpu_text_enabled")),
             "profiling must expose that speaker labels use the Vulkan text pass");
    QVERIFY2(profiling.contains(QStringLiteral("vulkan_transcript_overlay_gpu_text_enabled")),
             "profiling must expose that transcript subtitles use the Vulkan text pass");
    QVERIFY2(profiling.contains(QStringLiteral("temporal_debug_overlay_enabled")) &&
                 profiling.contains(QStringLiteral("temporal_debug_overlay_text")),
             "profiling must expose temporal debug overlay state");

    const QString textRenderer = readSourceFile(QStringLiteral("vulkan_text_renderer.cpp"));
    QVERIFY2(!textRenderer.isEmpty(), "vulkan_text_renderer.cpp must be readable");
    QVERIFY2(textRenderer.contains(QStringLiteral("VulkanTextPipeline")),
             "speaker labels must use the dedicated Vulkan text pipeline");
    QVERIFY2(textRenderer.contains(QStringLiteral("drawSpeakerLabel")),
             "speaker labels must be drawn by the Vulkan text renderer");
    QVERIFY2(textRenderer.contains(QStringLiteral("drawTranscriptOverlay")),
             "transcript subtitles must be drawn by the Vulkan text renderer");
    QVERIFY2(textRenderer.contains(QStringLiteral("prepareTranscriptOverlayAtlas")),
             "transcript glyph atlas upload must be available before the render pass");
    QVERIFY2(textRenderer.contains(QStringLiteral("prepareSpeakerLabelAtlas")),
             "speaker glyph atlas upload must be available before the render pass");
    QVERIFY2(textRenderer.contains(QStringLiteral("cachedResolvedFontFace")),
             "Vulkan text rendering must cache fontconfig face resolution outside the steady-state frame path");
    QVERIFY2(textRenderer.contains(QStringLiteral("if (m_speakerLayoutCache.valid && m_speakerLayoutCache.layoutKey == layoutKey)")),
             "speaker label text layout must be reused when its inputs are unchanged");
    QVERIFY2(textRenderer.contains(QStringLiteral("if (m_transcriptLayoutCache.valid && m_transcriptLayoutCache.layoutKey == layoutKey)")),
             "transcript text layout must be reused when its inputs are unchanged");
    QVERIFY2(textRenderer.contains(QStringLiteral("const SpeakerLayoutCache* layout = speakerLabelLayout(outputSize, spec)")),
             "speaker label draw must consume the cached prepared layout instead of rebuilding glyphs every frame");
    QVERIFY2(textRenderer.contains(QStringLiteral("const TranscriptLayoutCache* cachedLayout")),
             "transcript draw must consume the cached prepared layout instead of rebuilding glyphs every frame");
    QVERIFY2(textRenderer.contains(QStringLiteral("clip.transcriptOverlay.showShadow && !active")),
             "highlighted active subtitle words must not receive black shadow glyphs in the live Vulkan text path");
    QVERIFY2(!textRenderer.contains(QStringLiteral("QPainter")),
             "Vulkan text renderer must not use Qt painter text rendering");

    QVERIFY2(backend.contains(QStringLiteral("drawTranscriptOverlay(cb")),
             "direct Vulkan preview must route transcript subtitles through the Vulkan text renderer");
    QVERIFY2(backend.contains(QStringLiteral("m_speakerTextRenderer")),
             "speaker labels and transcript subtitles must not share one mutable glyph atlas image");
    QVERIFY2(backend.contains(QStringLiteral("m_temporalDebugTextRenderer")),
             "temporal debug overlay must have a dedicated Vulkan text renderer so it does not evict speaker/subtitle text atlases");
    QVERIFY2(backend.contains(QStringLiteral("temporalDebugOverlayText")) &&
                 backend.contains(QStringLiteral("TEMPORAL DEBUG")),
             "direct Vulkan preview must draw the temporal debug overlay through the Vulkan text path");
    QVERIFY2(backend.contains(QStringLiteral("prepareTranscriptOverlayAtlas(cb")) &&
                 backend.indexOf(QStringLiteral("prepareTranscriptOverlayAtlas(cb")) <
                     backend.indexOf(QStringLiteral("vkCmdBeginRenderPass")),
             "transcript glyph atlas upload must be recorded before vkCmdBeginRenderPass");
    QVERIFY2(backend.contains(QStringLiteral("transcriptFrameForClipSourceFrame(effectiveClip, status->presentedSourceFrame)")),
             "live Vulkan transcript subtitles must time against the presented video frame when one is available");
    QVERIFY2(!backend.contains(QStringLiteral("renderTranscriptOverlay(")),
             "direct Vulkan preview must not retain a CPU-rendered transcript overlay path");
}

void TestDirectVulkanHandoffPipelineContract::visibleDecodePriorityUsesTimelineDomain()
{
    const QString requests = readSourceFile(QStringLiteral("timeline_cache_requests.cpp"));
    QVERIFY2(!requests.isEmpty(), "timeline_cache_requests.cpp must be readable");
    QVERIFY2(requests.contains(QStringLiteral("calculatePriority(info, canonicalFrame)")),
             "visible decode priority must convert media source frames back to timeline-frame distance");
    QVERIFY2(!requests.contains(QStringLiteral("calculatePriority(canonicalFrame)")),
             "visible decode priority must not compare source-frame numbers directly to the timeline playhead");

    const QString cache = readSourceFile(QStringLiteral("timeline_cache.cpp"));
    QVERIFY2(!cache.isEmpty(), "timeline_cache.cpp must be readable");
    QVERIFY2(cache.contains(QStringLiteral("approximateTimelineFrameForClipSourceFrame(info.clip, sourceFrame)")),
             "timeline cache source-frame priority overload must use the shared timing-domain conversion helper");
    QVERIFY2(cache.contains(QStringLiteral("calculatePriority(info, targetFrame)")),
             "lead prefetch priority must use the same source-frame to timeline-frame conversion as visible decode");

    const QString timingHeader = readSourceFile(QStringLiteral("editor_shared_render_sync.h"));
    QVERIFY2(!timingHeader.isEmpty(), "editor_shared_render_sync.h must be readable");
    QVERIFY2(timingHeader.contains(QStringLiteral("approximateTimelineFrameForClipSourceFrame")),
             "source-frame to timeline-frame priority conversion must live in the shared timing helpers");

    const QString surface = readSourceFile(QStringLiteral("vulkan_preview_surface.cpp"));
    QVERIFY2(!surface.isEmpty(), "vulkan_preview_surface.cpp must be readable");
    QVERIFY2(surface.contains(QStringLiteral("effectivePlaybackLookaheadFrames()")) &&
                 surface.contains(QStringLiteral("targetSample + frameToSamples(offset)")),
             "playback advance must schedule visible future frames, not just chase the current audio-clock frame");
    QVERIFY2(surface.contains(QStringLiteral("dispatch_current_over_backlog")),
             "exact/current visible frame requests must be allowed through a saturated lookahead backlog");
    QVERIFY2(surface.contains(QStringLiteral("if (!targetExact)")) &&
                 surface.contains(QStringLiteral("if (!targetDisplayable)")) &&
                 surface.contains(QStringLiteral("continue;")) &&
                 surface.contains(QStringLiteral("for (int offset = 1; offset <= lookaheadFrames; ++offset)")),
             "lookahead scheduling must request exact target frames while only gating readiness on displayable frames");

    const QString decoder = readSourceFile(QStringLiteral("async_decoder.cpp"));
    QVERIFY2(!decoder.isEmpty(), "async_decoder.cpp must be readable");
    QVERIFY2(decoder.contains(QStringLiteral("queued.kind == DecodeRequestKind::Visible")) &&
                 decoder.contains(QStringLiteral("req.kind == DecodeRequestKind::Visible")) &&
                 decoder.contains(QStringLiteral("continue;")),
             "visible lookahead frames must coexist; newer visible frames must not supersede older visible frames");

    const QString cacheSource = readSourceFile(QStringLiteral("timeline_cache.cpp"));
    QVERIFY2(cacheSource.contains(QStringLiteral("effectiveVisibleDecodeKeepWindow()")),
             "visible decode cancel-before retention must be centralized in one adaptive policy");
    QVERIFY2(cacheSource.contains(QStringLiteral("visibleDecodeRetentionPolicySnapshot")),
             "visible decode retention policy must be exposed for REST/perf diagnosis");
    QVERIFY2(cacheSource.contains(QStringLiteral("kVisibleDecodeBaseKeepFrames = 96")) &&
                 cacheSource.contains(QStringLiteral("kVisibleDecodeMaxKeepFrames = 240")),
             "visible decode retention must retain the proven baseline while allowing bounded adaptation");

    const QString cacheRequests = readSourceFile(QStringLiteral("timeline_cache_requests.cpp"));
    QVERIFY2(cacheRequests.contains(QStringLiteral("effectiveVisibleDecodeKeepWindow()")),
             "active visible request cancellation must use the same adaptive policy as cache playback resync");
    QVERIFY2(!cacheRequests.contains(QStringLiteral("kVisibleDecodeKeepWindow")),
             "timeline_cache_requests.cpp must not carry a second visible decode keep-window constant");
    QVERIFY2(cacheRequests.contains(QStringLiteral("previewMaxPlaybackStaleFrameDelta(sourceFps")) &&
                 cacheRequests.contains(QStringLiteral("previewFrameIsTooStaleForPlayback(frame, frameNumber, maxStaleFrameDelta)")),
             "cache displayability must reject stale approximate playback frames with the shared source-rate-aware preview stale-frame policy");
    QVERIFY2(cacheRequests.contains(QStringLiteral("hasExactFrameForPreview")),
             "visible decode scheduling must be able to distinguish exact residency from approximate displayability");
    QVERIFY2(cacheRequests.contains(QStringLiteral("\"retention_policy\"")) ||
                 cacheRequests.contains(QStringLiteral("retention_policy")),
             "visible decode diagnostics must include the retention policy that made cancellation decisions");

    const QString selectionHeader = readSourceFile(QStringLiteral("preview_frame_selection.h"));
    QVERIFY2(selectionHeader.contains(QStringLiteral("kPreviewMaxPlaybackStaleSeconds = 0.20")) &&
                 selectionHeader.contains(QStringLiteral("previewMaxPlaybackStaleFrameDelta")) &&
                 selectionHeader.contains(QStringLiteral("previewFrameIsTooStaleForPlayback")),
             "preview stale-frame tolerance must have one shared source-rate-aware source of truth");

    const QString editorPlayback = readSourceFile(QStringLiteral("editor_playback.cpp"));
    QVERIFY2(!editorPlayback.isEmpty(), "editor_playback.cpp must be readable");
    QVERIFY2(editorPlayback.contains(QStringLiteral("m_preview->setPlaybackSpeed(m_playbackSpeed)")),
             "preview decode retention must be driven by the editor playback speed source of truth");

    const QString vulkanSurface = readSourceFile(QStringLiteral("vulkan_preview_surface.cpp"));
    QVERIFY2(vulkanSurface.contains(QStringLiteral("m_cache->setPlaybackSpeed(m_playbackSpeed)")),
             "Vulkan preview cache must receive the current playback speed");
    QVERIFY2(vulkanSurface.contains(QStringLiteral("previewMaxPlaybackStaleFrameDelta(resolvedSourceFps(clip))")) &&
                 vulkanSurface.contains(QStringLiteral("previewFrameIsTooStaleForPlayback(")) &&
                 vulkanSurface.contains(QStringLiteral("stale_hardware_frame_rejected")),
             "Vulkan direct preview must reject stale approximate hardware frames instead of handing them off");
    QVERIFY2(vulkanSurface.contains(QStringLiteral("displayableCached")) &&
                 vulkanSurface.contains(QStringLiteral("exactCached")) &&
                 vulkanSurface.contains(QStringLiteral("exact_frame_already_cached")),
             "visible decode scheduling must keep requesting exact frames even when an approximate frame is displayable");
    QVERIFY2(vulkanSurface.contains(QStringLiteral("debugTemporalDebugOverlayEnabled()")) &&
                 vulkanSurface.contains(QStringLiteral("temporalDebugOverlayText")),
             "Vulkan preview must populate the temporal debug overlay from the same frame-status state used by REST diagnostics");

    const QString debugControls = readSourceFile(QStringLiteral("debug_controls.cpp"));
    QVERIFY2(debugControls.contains(QStringLiteral("temporal_debug_overlay")),
             "temporal debug overlay must be controllable through the existing /debug options");
}

void TestDirectVulkanHandoffPipelineContract::schedulingDiagnosticsExposeRequiredFields()
{
    const QString profiling = readSourceFile(QStringLiteral("vulkan_preview_surface_profiling.cpp"));
    QVERIFY2(!profiling.isEmpty(), "vulkan_preview_surface_profiling.cpp must be readable");
    const QStringList previewFields{
        QStringLiteral("playback_smoothness"),
        QStringLiteral("visible_decode_diagnostics"),
        QStringLiteral("cache_pending_visible_requests"),
        QStringLiteral("pending_visible_requests"),
        QStringLiteral("decoder_diagnostics"),
        QStringLiteral("visible_request_attempts"),
        QStringLiteral("visible_request_dispatched"),
        QStringLiteral("visible_request_blocked"),
        QStringLiteral("visible_request_null_callbacks"),
        QStringLiteral("last_visible_request_block_reason"),
        QStringLiteral("last_visible_request_exact_cached"),
        QStringLiteral("last_visible_request_displayable_cached"),
        QStringLiteral("active_frame_up_to_date"),
        QStringLiteral("active_frame_not_up_to_date_failure"),
        QStringLiteral("current_frame_failure_rate"),
        QStringLiteral("active_frame_stale_rejected"),
        QStringLiteral("retention_policy"),
        QStringLiteral("vulkan_visible_decode_requires_direct_vulkan_payload"),
        QStringLiteral("vulkan_visible_cpu_upload_fallback_enabled")
    };
    for (const QString& field : previewFields) {
        QVERIFY2(profiling.contains(field),
                 qPrintable(QStringLiteral("preview perf diagnostics must expose %1").arg(field)));
    }

    const QString audio = readSourceFile(QStringLiteral("audio_engine.h"));
    QVERIFY2(!audio.isEmpty(), "audio_engine.h must be readable");
    const QStringList audioFields{
        QStringLiteral("audio_clock_available"),
        QStringLiteral("ring_buffer_frames_available"),
        QStringLiteral("ring_buffer_ms_available"),
        QStringLiteral("buffered_timeline_frames"),
        QStringLiteral("underrun_count"),
        QStringLiteral("last_callback_underrun_samples"),
        QStringLiteral("time_stretch_readiness_state"),
        QStringLiteral("time_stretch_generation_progress"),
        QStringLiteral("time_stretch_sidecar_only"),
        QStringLiteral("pitch_preserving_audio_blocked"),
        QStringLiteral("audio_playback_blocked"),
        QStringLiteral("stream_open"),
        QStringLiteral("stream_running")
    };
    for (const QString& field : audioFields) {
        QVERIFY2(audio.contains(field),
                 qPrintable(QStringLiteral("audio diagnostics must expose %1").arg(field)));
    }

    const QString routes = readSourceFile(QStringLiteral("control_server_worker_routes.cpp"));
    QVERIFY2(!routes.isEmpty(), "control_server_worker_routes.cpp must be readable");
    QVERIFY2(routes.contains(QStringLiteral("/audio")),
             "REST API must expose audio loading/buffering state through /audio");
    QVERIFY2(routes.contains(QStringLiteral("/pipeline")),
             "REST API must expose preview decode/presentation scheduling state through /pipeline");
}

void TestDirectVulkanHandoffPipelineContract::pipelineDiagnosticsDefaultToCompactSnapshot()
{
    const QString routes = readSourceFile(QStringLiteral("control_server_worker_routes.cpp"));
    QVERIFY2(!routes.isEmpty(), "control_server_worker_routes.cpp must be readable");
    QVERIFY2(routes.contains(QStringLiteral("queryBool(query, QStringLiteral(\"verbose\"))")),
             "/pipeline must require an explicit verbose query for rich debug data");
    QVERIFY2(routes.contains(QStringLiteral("refreshPipelineSnapshotFromUi(m_uiInvokeTimeoutMs, verbose")),
             "/pipeline must pass the requested diagnostic detail level through the control boundary");

    const QString editorProfiling = readSourceFile(QStringLiteral("editor_profiling.cpp"));
    QVERIFY2(!editorProfiling.isEmpty(), "editor_profiling.cpp must be readable");
    QVERIFY2(editorProfiling.contains(QStringLiteral("m_preview->pipelineHealthSnapshot()")),
             "default /pipeline must use the compact health snapshot");
    QVERIFY2(editorProfiling.contains(QStringLiteral("m_preview->profilingSnapshot()")) &&
                 editorProfiling.contains(QStringLiteral("if (verbose)")),
             "full profiling snapshot must remain explicit and verbose-only");

    const QString previewSurface = readSourceFile(QStringLiteral("preview_surface.h"));
    QVERIFY2(previewSurface.contains(QStringLiteral("pipelineHealthSnapshot")),
             "compact pipeline health must be a preview-surface contract");

    const QString vulkanProfiling = readSourceFile(QStringLiteral("vulkan_preview_surface_profiling.cpp"));
    QVERIFY2(vulkanProfiling.contains(QStringLiteral("QJsonObject VulkanPreviewSurface::pipelineHealthSnapshot() const")),
             "Vulkan preview must implement the compact pipeline health snapshot");
    QVERIFY2(vulkanProfiling.contains(QStringLiteral("pipelineStageHealthJson(livePipelineSnapshots())")),
             "compact /pipeline must expose named decode-to-preview stage state");
    QVERIFY2(vulkanProfiling.contains(QStringLiteral("decoder_diagnostics")),
             "compact /pipeline must expose decoder diagnostics needed to distinguish decode starvation");
    QVERIFY2(!vulkanProfiling.mid(vulkanProfiling.indexOf(QStringLiteral("QJsonObject VulkanPreviewSurface::pipelineHealthSnapshot() const")),
                                  vulkanProfiling.indexOf(QStringLiteral("void VulkanPreviewSurface::resetProfilingStats()")) -
                                      vulkanProfiling.indexOf(QStringLiteral("QJsonObject VulkanPreviewSurface::pipelineHealthSnapshot() const")))
                  .contains(QStringLiteral("currentSpeakerLabelDebugForState")),
             "compact pipeline polling must not perform speaker/transcript debug lookup");

    const QString vulkanSurface = readSourceFile(QStringLiteral("vulkan_preview_surface.cpp"));
    QVERIFY2(vulkanSurface.contains(QStringLiteral("13 Diagnostic Readback")) &&
                 vulkanSurface.contains(QStringLiteral("diagnostic_disabled")),
             "pipeline stages must report diagnostic readback as opt-in, not as a hot-path render dependency");
}

void TestDirectVulkanHandoffPipelineContract::pitchPreservingAudioUsesExplicitSidecarGate()
{
    const QString playback = readSourceFile(QStringLiteral("editor_playback.cpp"));
    QVERIFY2(!playback.isEmpty(), "editor_playback.cpp must be readable");
    QVERIFY2(playback.contains(QStringLiteral("needsPitchPreservingPlaybackAudio()")),
             "playback must centralize the decision to require pitch-preserving audio");
    QVERIFY2(playback.contains(QStringLiteral("playbackAudioReadyForFrame(m_timeline->currentFrame())")),
             "playback startup must gate on the exact retimed audio needed at the current frame");
    QVERIFY2(playback.contains(QStringLiteral("requestPlaybackAudioWarmup(true)")),
             "missing retimed audio must enter an explicit warmup/generation path");
    QVERIFY2(playback.contains(QStringLiteral("Audio being generated")),
             "preview overlay must make retimed audio generation visible to the user");
    QVERIFY2(playback.contains(QStringLiteral("Loading re-timed audio")),
             "preview overlay must make retimed audio loading visible to the user");

    const QString audio = readSourceFile(QStringLiteral("audio_engine.h"));
    QVERIFY2(!audio.isEmpty(), "audio_engine.h must be readable");
    QVERIFY2(audio.contains(QStringLiteral("snapshot[QStringLiteral(\"time_stretch_sidecar_only\")] = true")),
             "audio diagnostics must expose that pitch-preserving playback is sidecar-only");
    QVERIFY2(audio.contains(QStringLiteral("playbackAudioNeedsRetimingForFrame")),
             "audio engine must expose whether required retimed audio needs generation");
    QVERIFY2(!audio.contains(QStringLiteral("SOLA")),
             "sidecar-only pitch-preserving playback must not retain an implicit SOLA fallback path");
}

void TestDirectVulkanHandoffPipelineContract::overlayWorkerKeepsNewestCoalescedRequest()
{
    const QString header = readSourceFile(QStringLiteral("vulkan_preview_surface.h"));
    QVERIFY2(!header.isEmpty(), "vulkan_preview_surface.h must be readable");
    QVERIFY2(header.contains(QStringLiteral("m_queuedFacestreamOverlaySnapshotKey")),
             "overlay worker must retain the newest coalesced request key instead of dropping it");
    QVERIFY2(header.contains(QStringLiteral("m_queuedFacestreamOverlayRequestClips")),
             "overlay worker must retain the newest coalesced request payload");

    const QString source = readSourceFile(QStringLiteral("vulkan_preview_surface_facedetections.cpp"));
    QVERIFY2(!source.isEmpty(), "vulkan_preview_surface_facedetections.cpp must be readable");
    QVERIFY2(source.contains(QStringLiteral("startFacestreamOverlaySnapshotWorker(")),
             "overlay worker launch must be factored so queued follow-up requests can reuse it");
    QVERIFY2(source.contains(QStringLiteral("m_queuedFacestreamOverlaySnapshotKey = requestKey")),
             "newer overlay requests must replace the bounded queued request slot");
    QVERIFY2(source.contains(QStringLiteral("launchQueuedRequest();")),
             "overlay worker completion must launch the queued follow-up request");
    QVERIFY2(source.contains(QStringLiteral("facestreamOverlaySnapshotApplyDecision(")) &&
                 source.contains(QStringLiteral("++m_facedetectionsOverlayWorkerDropped")),
             "stale overlay worker result policy must be centralized and dropped results counted");

    const QString profiling = readSourceFile(QStringLiteral("vulkan_preview_surface_profiling.cpp"));
    QVERIFY2(!profiling.isEmpty(), "vulkan_preview_surface_profiling.cpp must be readable");
    QVERIFY2(profiling.contains(QStringLiteral("vulkan_overlay_worker_queued_key")),
             "perf diagnostics must expose the queued overlay worker request key");
    QVERIFY2(profiling.contains(QStringLiteral("vulkan_overlay_worker_queued_clip_count")),
             "perf diagnostics must expose the queued overlay worker request size");
}

void TestDirectVulkanHandoffPipelineContract::rendererConsumesLatchedPreviewSnapshot()
{
    const QString backend = readSourceFile(QStringLiteral("direct_vulkan_preview_backend.cpp"));
    QVERIFY2(!backend.isEmpty(), "direct_vulkan_preview_backend.cpp must be readable");

    const qsizetype liveStateIndex = backend.indexOf(QStringLiteral("const PreviewInteractionState* liveState = m_owner->state();"));
    const qsizetype snapshotIndex = backend.indexOf(QStringLiteral("PreviewInteractionState renderSnapshot;"), liveStateIndex);
    const qsizetype copyIndex = backend.indexOf(QStringLiteral("renderSnapshot = *liveState;"), snapshotIndex);
    const qsizetype stateAliasIndex = backend.indexOf(QStringLiteral("const PreviewInteractionState* state = liveState ? &renderSnapshot : nullptr;"), copyIndex);
    QVERIFY2(liveStateIndex >= 0 && snapshotIndex > liveStateIndex && copyIndex > snapshotIndex && stateAliasIndex > copyIndex,
             "direct Vulkan command recording must consume a stack-latched PreviewInteractionState snapshot");
}

void TestDirectVulkanHandoffPipelineContract::vulkanTextShaderUsesVulkanFramebufferYConvention()
{
    const QString shader = readSourceFile(QStringLiteral("shaders/vulkan/text.vert"));
    QVERIFY2(!shader.isEmpty(), "text.vert must be readable");

    const qsizetype topLeftPos = shader.indexOf(QStringLiteral("pos = vec2(-1.0, -1.0);"));
    const qsizetype topLeftUv = shader.indexOf(QStringLiteral("unitUv = vec2(0.0, 0.0);"), topLeftPos);
    QVERIFY2(topLeftPos >= 0 && topLeftUv > topLeftPos,
             "top-left Vulkan framebuffer vertex must sample the top-left glyph atlas UV");

    const qsizetype bottomRightPos = shader.indexOf(QStringLiteral("pos = vec2(1.0, 1.0);"));
    const qsizetype bottomRightUv = shader.indexOf(QStringLiteral("unitUv = vec2(1.0, 1.0);"), bottomRightPos);
    QVERIFY2(bottomRightPos >= 0 && bottomRightUv > bottomRightPos,
             "bottom-right Vulkan framebuffer vertex must sample the bottom-right glyph atlas UV");

    QVERIFY2(!shader.contains(QStringLiteral("pos = vec2(-1.0, -1.0);\n        unitUv = vec2(0.0, 1.0);")),
             "text shader must not use OpenGL-style Y-flipped glyph UVs in the Vulkan presenter");
}

QTEST_MAIN(TestDirectVulkanHandoffPipelineContract)
#include "test_direct_vulkan_handoff_pipeline_contract.moc"
