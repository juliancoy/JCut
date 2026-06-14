#include <QtTest/QtTest>

#include <QFile>
#include <QString>

class TestDirectVulkanHandoffPipelineContract : public QObject {
  Q_OBJECT

private slots:
  void directPreviewUsesExtractedPipelineBeforeRenderPass();
  void directPreviewRecordsTextureUploadsBeforeRenderPass();
  void directPreviewUsesSubmitBasedGpuHandoffApis();
  void directPreviewUsesPerClipHandoffDescriptors();
  void directPreviewRequiresHardwarePayloadsFromCache();
  void handoffPipelineRejectsCpuOnlyFrames();
  void strictDisplayabilityDoesNotAcceptCpuFallback();
  void directPreviewDisablesCpuAndQtTextOverlayFallbacks();
  void visibleDecodePriorityUsesTimelineDomain();
  void schedulingDiagnosticsExposeRequiredFields();
  void streamTimingDiagnosticsExposeClockDomains();
  void pipelineDiagnosticsDefaultToCompactSnapshot();
  void pitchPreservingAudioUsesExplicitSidecarGate();
  void noProxyHardwarePathIsPrimaryAndHoldsLateFrames();
  void overlayWorkerKeepsNewestCoalescedRequest();
  void facestreamTrackBoxesAreNotBaselinePlaybackWork();
  void playbackFacestreamOverlaysDoNotColdLoadOnPresentationPath();
  void rendererConsumesLatchedPreviewSnapshot();
  void exportSpeakerLabelUsesFractionalMasterClockPosition();
  void speakerFramingUsesRenderSyncMarkersInPreviewAndExport();
  void speakerFramingAndExportUseFractionalFitGeometry();
  void contiguousTranscriptSectionsCanHoldMultipleTracks();
  void trackAssignmentDoesNotCreateFaceBoxKeyframes();
  void speechFilterBlendUsesPrecomputedSampleRanges();
  void vulkanTextShaderUsesVulkanFramebufferYConvention();
};

namespace {

QString readSourceFile(const QString &relativePath) {
  QFile file(QStringLiteral(JCUT_SOURCE_DIR) + QLatin1Char('/') + relativePath);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return {};
  }
  return QString::fromUtf8(file.readAll());
}

QString readSourceFiles(const QStringList &relativePaths) {
  QString combined;
  for (const QString &path : relativePaths) {
    const QString source = readSourceFile(path);
    if (source.isEmpty()) {
      return {};
    }
    combined += source;
    combined += QLatin1Char('\n');
  }
  return combined;
}

} // namespace

void TestDirectVulkanHandoffPipelineContract::
    directPreviewUsesExtractedPipelineBeforeRenderPass() {
  const QString source =
      readSourceFile(QStringLiteral("direct_vulkan_preview_window.cpp"));
  QVERIFY2(!source.isEmpty(),
           "direct_vulkan_preview_window.cpp must be readable");

  const qsizetype recordIndex =
      source.indexOf(QStringLiteral("handoffResources->pipeline->record("));
  const qsizetype beginRenderPassIndex =
      source.indexOf(QStringLiteral("vkCmdBeginRenderPass"));
  QVERIFY2(recordIndex >= 0,
           "direct preview must call the extracted frame handoff pipeline");
  QVERIFY2(beginRenderPassIndex >= 0,
           "direct preview must explicitly begin its render pass");
  QVERIFY2(recordIndex < beginRenderPassIndex,
           "handoff transfer/compute recording must happen before "
           "vkCmdBeginRenderPass");
}

void TestDirectVulkanHandoffPipelineContract::
    directPreviewRecordsTextureUploadsBeforeRenderPass() {
  const QString source =
      readSourceFile(QStringLiteral("direct_vulkan_preview_window.cpp"));
  QVERIFY2(!source.isEmpty(),
           "direct_vulkan_preview_window.cpp must be readable");

  const qsizetype beginRenderPassIndex =
      source.indexOf(QStringLiteral("vkCmdBeginRenderPass"));
  QVERIFY2(beginRenderPassIndex >= 0,
           "direct preview must explicitly begin its render pass");

  const QStringList uploadMarkers{
      QStringLiteral("uploadCurveLut(cb,"),
      QStringLiteral("uploadImageTexture(cb, overlayImage)")};
  for (const QString &marker : uploadMarkers) {
    qsizetype index = source.indexOf(marker);
    QVERIFY2(index >= 0,
             qPrintable(
                 QStringLiteral("direct preview must contain %1").arg(marker)));
    while (index >= 0) {
      QVERIFY2(index < beginRenderPassIndex,
               qPrintable(QStringLiteral(
                              "%1 must be recorded before vkCmdBeginRenderPass")
                              .arg(marker)));
      index = source.indexOf(marker, index + marker.size());
    }
  }
}

void TestDirectVulkanHandoffPipelineContract::
    directPreviewUsesSubmitBasedGpuHandoffApis() {
  const QString previewSource =
      readSourceFile(QStringLiteral("direct_vulkan_preview_window.cpp"));
  const QString pipelineSource =
      readSourceFile(QStringLiteral("direct_vulkan_frame_handoff_pipeline.cpp"));
  QVERIFY2(!previewSource.isEmpty(),
           "direct_vulkan_preview_window.cpp must be readable");
  QVERIFY2(!pipelineSource.isEmpty(),
           "direct_vulkan_frame_handoff_pipeline.cpp must be readable");

  QVERIFY2(
      pipelineSource.contains(QStringLiteral("m_handoff->uploadFrame(")),
      "direct preview handoff must use the owned uploadFrame submit path so "
      "CUDA/Vulkan semaphore waits can be attached to the handoff submit");
  QVERIFY2(
      pipelineSource.contains(QStringLiteral("m_handoff->importOffscreenFrame(")),
      "direct preview external Vulkan handoff must use the owned import submit "
      "path rather than recording into Qt's caller-owned submit");
  QVERIFY2(
      pipelineSource.contains(QStringLiteral("uploadFrame(status.frame, false")),
      "direct preview must disable VulkanDetectorFrameHandoff CPU upload "
      "fallback; visible frames require hardware/external GPU payloads");
}

void TestDirectVulkanHandoffPipelineContract::
    directPreviewUsesPerClipHandoffDescriptors() {
  const QString backend =
      readSourceFile(QStringLiteral("direct_vulkan_preview_window.cpp"));
  QVERIFY2(!backend.isEmpty(),
           "direct_vulkan_preview_window.cpp must be readable");

  QVERIFY2(
      backend.contains(QStringLiteral(
          "QHash<QString, std::shared_ptr<ClipHandoffResources>> "
          "m_clipHandoffResources")),
      "direct preview must own separate handoff resources per active clip");
  QVERIFY2(
      backend.contains(QStringLiteral("QVector<RetiredClipHandoffResources> "
                                      "m_retiredClipHandoffResources")),
      "inactive handoff resources must be retired briefly instead of destroyed "
      "while swapchain frames may still be in flight");
  QVERIFY2(backend.contains(
               QStringLiteral("ensureClipHandoffResources(status.clipId)")),
           "direct preview must resolve handoff resources by clip id");
  QVERIFY2(backend.contains(QStringLiteral(
               "pruneClipHandoffResources(activeHandoffClipIds)")),
           "direct preview must release per-clip handoff resources when clips "
           "leave the active render set");
  QVERIFY2(backend.contains(QStringLiteral(
               "static_cast<int>(VulkanResources::kDescriptorSetCount) + 1")),
           "retired handoff resources must stay alive for at least the "
           "descriptor ring depth");
  QVERIFY2(backend.contains(QStringLiteral("handoffResult.descriptorSet")),
           "clip draws must bind the descriptor set captured by that clip's "
           "handoff result");
  QVERIFY2(backend.contains(QStringLiteral("activeClipHandoffResourceCount")),
           "direct preview diagnostics must expose active per-clip handoff "
           "resource ownership");
  QVERIFY2(backend.contains(QStringLiteral("retiredClipHandoffResourceCount")),
           "direct preview diagnostics must expose retired in-flight handoff "
           "resource ownership");
  QVERIFY2(!backend.contains(
               QStringLiteral("multi_clip_handoff_requires_descriptor_pool")),
           "multiple active clips must not be rejected due to a shared "
           "sampled-image descriptor");

  const QString surface =
      readSourceFile(QStringLiteral("vulkan_preview_surface.cpp"));
  QVERIFY2(!surface.isEmpty(), "vulkan_preview_surface.cpp must be readable");
  QVERIFY2(
      surface.contains(QStringLiteral("active_clip_handoff_resource_count")) &&
          surface.contains(
              QStringLiteral("retired_clip_handoff_resource_count")),
      "stage 11 diagnostics must include active and retired handoff resource "
      "ownership");

  const QString presenter =
      readSourceFile(QStringLiteral("direct_vulkan_preview_presenter.cpp"));
  QVERIFY2(!presenter.isEmpty(),
           "direct_vulkan_preview_presenter.cpp must be readable");
  QVERIFY2(
      presenter.contains(QStringLiteral("active_clip_handoff_resource_count")),
      "presenter diagnostics must expose active per-clip handoff resource "
      "count");
  QVERIFY2(
      presenter.contains(QStringLiteral("retired_clip_handoff_resource_count")),
      "presenter diagnostics must expose retired per-clip handoff resource "
      "count");
  QVERIFY2(presenter.contains(QStringLiteral("explicit_failure_draw_count")),
           "compact presenter diagnostics must expose explicit failure draws");

  const QString pipeline =
      readSourceFile(QStringLiteral("direct_vulkan_frame_handoff_pipeline.h"));
  QVERIFY2(!pipeline.isEmpty(),
           "direct_vulkan_frame_handoff_pipeline.h must be readable");
  QVERIFY2(pipeline.contains(QStringLiteral("VkDescriptorSet descriptorSet")),
           "handoff result must carry the descriptor set whose sampled image "
           "it updated");

  const QString resources =
      readSourceFile(QStringLiteral("vulkan_resources.h"));
  QVERIFY2(
      resources.contains(
          QStringLiteral("static constexpr size_t kDescriptorSetCount")),
      "descriptor ring depth must be visible to the presenter lifetime policy");
}

void TestDirectVulkanHandoffPipelineContract::
    directPreviewRequiresHardwarePayloadsFromCache() {
  const QString source =
      readSourceFile(QStringLiteral("vulkan_preview_surface.cpp"));
  QVERIFY2(!source.isEmpty(), "vulkan_preview_surface.cpp must be readable");

  QVERIFY2(source.contains(QStringLiteral(
               "constexpr bool requireDirectVulkanPayload = true;")),
           "direct Vulkan preview must require hardware/GPU payloads for "
           "visible frames; CPU upload fallback is rejected");
  QVERIFY2(
      source.contains(QStringLiteral("requireDirectVulkanPayload);")),
      "visible frame payload policy must be passed into frame requests");
}

void TestDirectVulkanHandoffPipelineContract::
    handoffPipelineRejectsCpuOnlyFrames() {
  const QString source = readSourceFile(
      QStringLiteral("direct_vulkan_frame_handoff_pipeline.cpp"));
  QVERIFY2(!source.isEmpty(),
           "direct_vulkan_frame_handoff_pipeline.cpp must be readable");

  QVERIFY2(
      source.contains(QStringLiteral("!status.externalVulkanFrame && !status.frame.hasHardwareFrame()")),
      "handoff pipeline must reject CPU-only frames before the handoff layer");
  QVERIFY2(!source.contains(QStringLiteral("QStringLiteral(\"cpu_upload\")")),
           "handoff pipeline must not report CPU upload as a render mode");
  QVERIFY2(source.contains(QStringLiteral("uploadFrame(status.frame, false")),
           "handoff pipeline must disable CPU image upload fallback");
}

void TestDirectVulkanHandoffPipelineContract::
    strictDisplayabilityDoesNotAcceptCpuFallback() {
  const QString source =
      readSourceFile(QStringLiteral("timeline_cache_requests.cpp"));
  QVERIFY2(!source.isEmpty(), "timeline_cache_requests.cpp must be readable");

  QVERIFY2(
      source.contains(QStringLiteral("requireHardwareOrGpuPayload &&")),
      "timeline cache must distinguish strict hardware/GPU preview requests");
  QVERIFY2(source.contains(QStringLiteral(
               "!frame.hasHardwareFrame() && !frame.hasGpuTexture()")),
           "strict visible preview must reject non-hardware/non-GPU payloads");
  QVERIFY2(source.contains(QStringLiteral("strictPayloadRejected")),
           "strict CPU-payload rejection must be counted for diagnostics");
}

void TestDirectVulkanHandoffPipelineContract::
    directPreviewDisablesCpuAndQtTextOverlayFallbacks() {
  const QString backend =
      readSourceFile(QStringLiteral("direct_vulkan_preview_window.cpp"));
  QVERIFY2(!backend.isEmpty(),
           "direct_vulkan_preview_window.cpp must be readable");
  QVERIFY2(backend.contains(QStringLiteral(
               "kAllowCpuRasterTextOverlaysInDirectVulkanPreview = false")),
           "direct Vulkan preview must not CPU-rasterize "
           "title/transcript/speaker/status text overlays");

  const QString presenter =
      readSourceFile(QStringLiteral("direct_vulkan_preview_presenter.cpp"));
  QVERIFY2(!presenter.isEmpty(),
           "direct_vulkan_preview_presenter.cpp must be readable");
  QVERIFY2(presenter.contains(QStringLiteral(
               "kAllowQtPainterOverlayInDirectVulkanPreview = false")),
           "direct Vulkan preview must not paint presentation overlays through "
           "Qt/QPainter");

  const QString profiling =
      readSourceFile(QStringLiteral("vulkan_preview_surface_profiling.cpp"));
  QVERIFY2(!profiling.isEmpty(),
           "vulkan_preview_surface_profiling.cpp must be readable");
  QVERIFY2(profiling.contains(
               QStringLiteral("vulkan_text_overlay_cpu_rasterization_enabled")),
           "profiling must expose the CPU text overlay contract");
  QVERIFY2(profiling.contains(
               QStringLiteral("vulkan_text_overlay_qt_painter_enabled")),
           "profiling must expose the Qt painter overlay contract");
  QVERIFY2(
      profiling.contains(
          QStringLiteral("vulkan_speaker_label_gpu_text_enabled")),
      "profiling must expose that speaker labels use the Vulkan text pass");
  QVERIFY2(profiling.contains(
               QStringLiteral("vulkan_transcript_overlay_gpu_text_enabled")),
           "profiling must expose that transcript subtitles use the Vulkan "
           "text pass");
  QVERIFY2(
      profiling.contains(QStringLiteral("temporal_debug_overlay_enabled")) &&
          profiling.contains(QStringLiteral("temporal_debug_overlay_text")),
      "profiling must expose temporal debug overlay state");

  const QString textRenderer =
      readSourceFile(QStringLiteral("vulkan_text_renderer.cpp"));
  QVERIFY2(!textRenderer.isEmpty(),
           "vulkan_text_renderer.cpp must be readable");
  QVERIFY2(textRenderer.contains(QStringLiteral("VulkanTextPipeline")),
           "speaker labels must use the dedicated Vulkan text pipeline");
  QVERIFY2(textRenderer.contains(QStringLiteral("drawSpeakerLabel")),
           "speaker labels must be drawn by the Vulkan text renderer");
  QVERIFY2(textRenderer.contains(QStringLiteral("drawTranscriptOverlay")),
           "transcript subtitles must be drawn by the Vulkan text renderer");
  QVERIFY2(
      textRenderer.contains(QStringLiteral("prepareTranscriptOverlayAtlas")),
      "transcript glyph atlas upload must be available before the render pass");
  QVERIFY2(
      textRenderer.contains(QStringLiteral("prepareSpeakerLabelAtlas")),
      "speaker glyph atlas upload must be available before the render pass");
  QVERIFY2(textRenderer.contains(QStringLiteral("cachedResolvedFontFace")),
           "Vulkan text rendering must cache fontconfig face resolution "
           "outside the steady-state frame path");
  QVERIFY2(
      textRenderer.contains(
          QStringLiteral("if (m_speakerLayoutCache.valid && "
                         "m_speakerLayoutCache.layoutKey == layoutKey)")),
      "speaker label text layout must be reused when its inputs are unchanged");
  QVERIFY2(
      textRenderer.contains(
          QStringLiteral("if (m_transcriptLayoutCache.valid && "
                         "m_transcriptLayoutCache.layoutKey == layoutKey)")),
      "transcript text layout must be reused when its inputs are unchanged");
  QVERIFY2(textRenderer.contains(
               QStringLiteral("const SpeakerLayoutCache* layout = "
                              "speakerLabelLayout(outputSize, spec)")),
           "speaker label draw must consume the cached prepared layout instead "
           "of rebuilding glyphs every frame");
  QVERIFY2(textRenderer.contains(
               QStringLiteral("const TranscriptLayoutCache* cachedLayout")),
           "transcript draw must consume the cached prepared layout instead of "
           "rebuilding glyphs every frame");
  QVERIFY2(textRenderer.contains(
               QStringLiteral("clip.transcriptOverlay.showShadow && !active")),
           "highlighted active subtitle words must not receive black shadow "
           "glyphs in the live Vulkan text path");
  QVERIFY2(!textRenderer.contains(QStringLiteral("QPainter")),
           "Vulkan text renderer must not use Qt painter text rendering");

  QVERIFY2(backend.contains(QStringLiteral("drawTranscriptOverlay(cb")),
           "direct Vulkan preview must route transcript subtitles through the "
           "Vulkan text renderer");
  QVERIFY2(backend.contains(QStringLiteral("m_speakerTextRenderer")),
           "speaker labels and transcript subtitles must not share one mutable "
           "glyph atlas image");
  QVERIFY2(backend.contains(QStringLiteral("m_temporalDebugTextRenderer")),
           "temporal debug overlay must have a dedicated Vulkan text renderer "
           "so it does not evict speaker/subtitle text atlases");
  QVERIFY2(backend.contains(QStringLiteral("temporalDebugOverlayText")) &&
               backend.contains(QStringLiteral("TEMPORAL DEBUG")),
           "direct Vulkan preview must draw the temporal debug overlay through "
           "the Vulkan text path");
  QVERIFY2(
      backend.contains(QStringLiteral("prepareTranscriptOverlayAtlas(cb")) &&
          backend.indexOf(QStringLiteral("prepareTranscriptOverlayAtlas(cb")) <
              backend.indexOf(QStringLiteral("vkCmdBeginRenderPass")),
      "transcript glyph atlas upload must be recorded before "
      "vkCmdBeginRenderPass");
  QVERIFY2(backend.contains(
               QStringLiteral("transcriptFrameForClipSourceFrame(effectiveClip,"
                              " status->presentedSourceFrame)")),
           "live Vulkan transcript subtitles must time against the presented "
           "video frame when one is available");
  QVERIFY2(!backend.contains(QStringLiteral("renderTranscriptOverlay(")),
           "direct Vulkan preview must not retain a CPU-rendered transcript "
           "overlay path");
}

void TestDirectVulkanHandoffPipelineContract::
    visibleDecodePriorityUsesTimelineDomain() {
  const QString requests =
      readSourceFile(QStringLiteral("timeline_cache_requests.cpp"));
  QVERIFY2(!requests.isEmpty(), "timeline_cache_requests.cpp must be readable");
  QVERIFY2(requests.contains(
               QStringLiteral("calculatePriority(info, canonicalFrame)")),
           "visible decode priority must convert media source frames back to "
           "timeline-frame distance");
  QVERIFY2(
      !requests.contains(QStringLiteral("calculatePriority(canonicalFrame)")),
      "visible decode priority must not compare source-frame numbers directly "
      "to the timeline playhead");

  const QString cache = readSourceFile(QStringLiteral("timeline_cache.cpp"));
  QVERIFY2(!cache.isEmpty(), "timeline_cache.cpp must be readable");
  QVERIFY2(cache.contains(QStringLiteral("approximateTimelineFrameForClipSource"
                                         "Frame(info.clip, sourceFrame)")),
           "timeline cache source-frame priority overload must use the shared "
           "timing-domain conversion helper");
  QVERIFY2(
      cache.contains(QStringLiteral("calculatePriority(info, targetFrame)")),
      "lead prefetch priority must use the same source-frame to timeline-frame "
      "conversion as visible decode");

  const QString timingHeader =
      readSourceFile(QStringLiteral("editor_shared_render_sync.h"));
  QVERIFY2(!timingHeader.isEmpty(),
           "editor_shared_render_sync.h must be readable");
  QVERIFY2(timingHeader.contains(
               QStringLiteral("approximateTimelineFrameForClipSourceFrame")),
           "source-frame to timeline-frame priority conversion must live in "
           "the shared timing helpers");

  const QString surface =
      readSourceFile(QStringLiteral("vulkan_preview_surface.cpp"));
  QVERIFY2(!surface.isEmpty(), "vulkan_preview_surface.cpp must be readable");
  QVERIFY2(surface.contains(
               QStringLiteral("m_playbackPipeline->requestFramesForSample")) &&
               surface.contains(QStringLiteral("playback_pipeline_window")),
           "active direct-Vulkan playback must route visible decode through "
           "PlaybackFramePipeline");
  const int prepareStart = surface.indexOf(QStringLiteral(
      "bool VulkanPreviewSurface::preparePlaybackAdvanceSample"));
  const int lookaheadStart = surface.indexOf(QStringLiteral(
      "bool VulkanPreviewSurface::hasPlaybackLookaheadBuffered"));
  QVERIFY2(prepareStart >= 0 && lookaheadStart > prepareStart,
           "direct-Vulkan playback readiness probe must be present and bounded "
           "for source inspection");
  const QString prepareBody =
      surface.mid(prepareStart, lookaheadStart - prepareStart);
  QVERIFY2(!prepareBody.contains(QStringLiteral("requestFramesForSample")),
           "preparePlaybackAdvanceSample must only check readiness; "
           "requestFramesForCurrentPosition is the single active scheduler");
  const int warmupStart = surface.indexOf(
      QStringLiteral("bool VulkanPreviewSurface::warmPlaybackLookahead"));
  QVERIFY2(
      warmupStart > lookaheadStart,
      "direct-Vulkan startup warmup must be present for source inspection");
  const QString warmupBody = surface.mid(warmupStart);
  QVERIFY2(warmupBody.contains(
               QStringLiteral("m_playbackPipeline->requestFramesForSample")) &&
               !warmupBody.contains(
                   QStringLiteral("requestFramesForCurrentPosition()")),
           "startup warmup must schedule PlaybackFramePipeline directly "
           "because playback state is not active yet");
  QVERIFY2(warmupBody.contains(QStringLiteral("warmFacestreamOverlayLookahead")),
           "startup warmup must also hydrate speaker/FaceDetections overlay "
           "buckets before playback begins");
  QVERIFY2(warmupBody.contains(
               QStringLiteral("m_playbackPipeline->setPlaybackActive(false)")),
           "failed startup warmup must unwind PlaybackFramePipeline active "
           "state before returning to the editor");
  QVERIFY2(!surface.contains(QStringLiteral("m_cache->startPrefetching()")) &&
               !surface.contains(
                   QStringLiteral("TimelineCache::PlaybackState::Playing")),
           "direct-Vulkan active playback must not start TimelineCache "
           "playback prefetch alongside PlaybackFramePipeline");

  const QString playbackPipeline =
      readSourceFile(QStringLiteral("playback_frame_pipeline.cpp"));
  QVERIFY2(!playbackPipeline.isEmpty(),
           "playback_frame_pipeline.cpp must be readable");
  QVERIFY2(playbackPipeline.contains(
               QStringLiteral("offset == 0 ? DecodeRequestKind::Visible")) &&
               playbackPipeline.contains(
                   QStringLiteral(": DecodeRequestKind::Prefetch")),
           "only the current playback sample is visible; future warmup must be "
           "prefetch");
  QVERIFY2(
      playbackPipeline.contains(QStringLiteral("recentVisibleWaitMs > 33")) &&
          playbackPipeline.contains(QStringLiteral(
              "pendingVisibleCount >= qMax(1, debugMaxVisibleBacklog())")) &&
          playbackPipeline.contains(QStringLiteral("latencyLeadFrames + 2")) &&
          playbackPipeline.contains(QStringLiteral("firstOffset")),
      "playback prefetch must become latency-sized future buffering when "
      "current visible decode is late or already pending");
  QVERIFY2(
      playbackPipeline.contains(QStringLiteral(
          "kind == DecodeRequestKind::Visible ? 100 : qMax(10, 60 - offset)")),
      "prefetch priority must be materially lower than current visible "
      "priority");

  const QString playbackDebugControls =
      readSourceFile(QStringLiteral("debug_controls.cpp"));
  QVERIFY2(!playbackDebugControls.isEmpty(), "debug_controls.cpp must be readable");
  QVERIFY2(playbackDebugControls.contains(
               QStringLiteral("kDefaultCancelBeforeMinFrameAdvance = 6")) &&
               playbackDebugControls.contains(
                   QStringLiteral("kDefaultCancelBeforeMinIntervalMs = 45")),
           "playback cancel-before throttling must not run at display-frame "
           "cadence");
  QVERIFY2(playbackDebugControls.contains(
               QStringLiteral("defaults.decodePreference = DecodePreference::Hardware;")),
           "project defaults should stay on portable hardware decode; the "
           "direct Vulkan surface applies its own zero-copy preview override");

  const QString vulkanSurface =
      readSourceFile(QStringLiteral("vulkan_preview_surface.cpp"));
  QVERIFY2(vulkanSurface.contains(QStringLiteral("DecodePreference::HardwareZeroCopy")) &&
               vulkanSurface.contains(QStringLiteral("m_forcedPreviewDecodePreference = true")),
           "direct Vulkan preview must force zero-copy handles for playback "
           "instead of materializing every frame as a CPU image");

  const QString decoder = readSourceFile(QStringLiteral("async_decoder.cpp"));
  QVERIFY2(!decoder.isEmpty(), "async_decoder.cpp must be readable");
  QVERIFY2(decoder.contains(
               QStringLiteral("queued.kind == DecodeRequestKind::Visible")) &&
               decoder.contains(
                   QStringLiteral("req.kind == DecodeRequestKind::Visible")) &&
               decoder.contains(QStringLiteral("continue;")),
           "visible requests must not be hidden by proximity supersession; "
           "stale cancellation must be explicit");

  const QString cacheSource =
      readSourceFile(QStringLiteral("timeline_cache.cpp"));
  QVERIFY2(cacheSource.contains(
               QStringLiteral("effectiveVisibleDecodeKeepWindow()")),
           "visible decode cancel-before retention must be centralized in one "
           "adaptive policy");
  QVERIFY2(cacheSource.contains(
               QStringLiteral("visibleDecodeRetentionPolicySnapshot")),
           "visible decode retention policy must be exposed for REST/perf "
           "diagnosis");
  QVERIFY2(cacheSource.contains(
               QStringLiteral("kVisibleDecodeBaseKeepFrames = 96")) &&
               cacheSource.contains(
                   QStringLiteral("kVisibleDecodeMaxKeepFrames = 240")),
           "visible decode retention must retain the proven baseline while "
           "allowing bounded adaptation");
  const int onDecodedStart =
      cacheSource.indexOf(QStringLiteral("void TimelineCache::onFrameDecoded"));
  const int memoryPressureStart = cacheSource.indexOf(
      QStringLiteral("void TimelineCache::onMemoryPressure"));
  QVERIFY2(onDecodedStart >= 0 && memoryPressureStart > onDecodedStart,
           "TimelineCache global decoder completion hook must be present and "
           "bounded for source inspection");
  const QString onDecodedBody =
      cacheSource.mid(onDecodedStart, memoryPressureStart - onDecodedStart);
  QVERIFY2(!onDecodedBody.contains(QStringLiteral("m_playbackBuffers")),
           "global decoder completions may seed cache, but must not populate "
           "TimelineCache playback buffers for PlaybackFramePipeline work");

  const QString cacheRequests =
      readSourceFile(QStringLiteral("timeline_cache_requests.cpp"));
  QVERIFY2(!cacheRequests.contains(QStringLiteral("kVisibleDecodeKeepWindow")),
           "timeline_cache_requests.cpp must not carry a second visible decode "
           "keep-window constant");
  QVERIFY2(
      playbackPipeline.contains(
          QStringLiteral("cancelDecoderBeforeThrottled")) &&
          playbackPipeline.contains(
              QStringLiteral("m_decoder->cancelForFileBefore")) &&
          playbackPipeline.contains(QStringLiteral("decoderKeepFromFrame")),
      "active playback visible request cancellation must live in "
      "PlaybackFramePipeline");
  QVERIFY2(cacheRequests.contains(
               QStringLiteral("previewMaxPlaybackStaleFrameDelta(sourceFps")) &&
               cacheRequests.contains(
                   QStringLiteral("previewFrameIsTooStaleForPlayback(frame, "
                                  "frameNumber, maxStaleFrameDelta)")),
           "cache displayability must reject stale approximate playback frames "
           "with the shared source-rate-aware preview stale-frame policy");
  QVERIFY2(cacheRequests.contains(QStringLiteral("hasExactFrameForPreview")),
           "visible decode scheduling must be able to distinguish exact "
           "residency from approximate displayability");
  QVERIFY2(cacheRequests.contains(QStringLiteral("\"retention_policy\"")) ||
               cacheRequests.contains(QStringLiteral("retention_policy")),
           "visible decode diagnostics must include the retention policy that "
           "made cancellation decisions");

  const QString selectionHeader =
      readSourceFile(QStringLiteral("preview_frame_selection.h"));
  QVERIFY2(selectionHeader.contains(
               QStringLiteral("kPreviewMaxPlaybackStaleSeconds = 0.067")) &&
               selectionHeader.contains(
                   QStringLiteral("previewMaxPlaybackStaleFrameDelta")) &&
               selectionHeader.contains(
                   QStringLiteral("previewFrameIsTooStaleForPlayback")),
           "preview stale-frame tolerance must have one shared "
           "source-rate-aware source of truth");

  const QString editorPlayback =
      readSourceFile(QStringLiteral("editor_playback.cpp"));
  QVERIFY2(!editorPlayback.isEmpty(), "editor_playback.cpp must be readable");
  QVERIFY2(editorPlayback.contains(
               QStringLiteral("m_preview->setPlaybackSpeed(m_playbackSpeed)")),
           "preview decode retention must be driven by the editor playback "
           "speed source of truth");

  QVERIFY2(vulkanSurface.contains(
               QStringLiteral("m_cache->setPlaybackSpeed(m_playbackSpeed)")),
           "Vulkan preview cache must receive the current playback speed");
  QVERIFY2(vulkanSurface.contains(QStringLiteral(
               "previewMaxPlaybackStaleFrameDelta(resolvedSourceFps(clip))")) &&
               vulkanSurface.contains(
                   QStringLiteral("previewFrameIsTooStaleForPlayback(")) &&
               vulkanSurface.contains(QStringLiteral(
                   "status.staleFrameRejected = selectedTooStale")),
           "Vulkan direct preview must diagnose stale approximate hardware "
           "frames without converting them into missing/black frames");
  QVERIFY2(
      vulkanSurface.contains(QStringLiteral("displayableCached")) &&
          vulkanSurface.contains(QStringLiteral("exactCached")) &&
          vulkanSurface.contains(QStringLiteral("exact_frame_already_cached")),
      "visible decode scheduling must keep requesting exact frames even when "
      "an approximate frame is displayable");
  QVERIFY2(
      vulkanSurface.contains(
          QStringLiteral("debugTemporalDebugOverlayEnabled()")) &&
          vulkanSurface.contains(QStringLiteral("temporalDebugOverlayText")),
      "Vulkan preview must populate the temporal debug overlay from the same "
      "frame-status state used by REST diagnostics");

  const QString debugControls =
      readSourceFile(QStringLiteral("debug_controls.cpp"));
  QVERIFY2(debugControls.contains(QStringLiteral("temporal_debug_overlay")),
           "temporal debug overlay must be controllable through the existing "
           "/debug options");

  const QString editorSource = readSourceFile(QStringLiteral("editor.cpp"));
  QVERIFY2(editorSource.contains(QStringLiteral(
               "debugDecodePreference = editor::DecodePreference::HardwareZeroCopy")),
           "loading a Vulkan project must force hardware-zero-copy after state "
           "load so NVIDIA decode is not materialized as CPU images");
  QVERIFY2(editorSource.contains(QStringLiteral(
               "debugDecodePreference == editor::DecodePreference::Software")) &&
               editorSource.contains(QStringLiteral(
                   "debugDecodePreference = editor::DecodePreference::Hardware")),
           "loading project state must sanitize legacy software decode mode "
           "back to hardware decode");
  const QString projectState = readSourceFile(QStringLiteral("project_state.cpp"));
  QVERIFY2(!projectState.contains(QStringLiteral("debugDecodeMode")),
           "project state must not persist decode mode; saved software decode "
           "must not disable NVIDIA playback decode on future runs");
  const QString inspectorTabs =
      readSourceFile(QStringLiteral("inspector_pane_secondary_tabs.cpp"));
  QVERIFY2(!inspectorTabs.contains(QStringLiteral("GPU Zero-Copy")) &&
               !inspectorTabs.contains(QStringLiteral("hardware_zero_copy")) &&
               !inspectorTabs.contains(QStringLiteral("CPU Software")) &&
               !inspectorTabs.contains(QStringLiteral("\"software\"")),
           "interactive decode controls must not expose hardware-zero-copy or "
           "CPU software decode");
  QVERIFY2(debugControls.contains(QStringLiteral(
               "if (preference == DecodePreference::Software)")) &&
               debugControls.contains(QStringLiteral(
                   "preference = DecodePreference::Hardware")),
           "runtime debug decode preference must reject CPU software decode");
  QVERIFY2(debugControls.contains(
               QStringLiteral("*preferenceOut = DecodePreference::HardwareZeroCopy;")),
           "explicit hardware-zero-copy state must survive parsing so direct "
           "Vulkan playback can request hardware frame handles");
  const QString decoderContext = readSourceFile(QStringLiteral("decoder_context.cpp"));
  QVERIFY2(decoderContext.contains(QStringLiteral("std::defer_lock")) &&
               decoderContext.contains(QStringLiteral("if (!m_info.hardwareAccelerated)")) &&
               decoderContext.contains(QStringLiteral("decodeLock.lock()")),
           "global FFmpeg decode serialization must apply only to software "
           "decode; NVIDIA hardware decode lanes must run concurrently");
  const QString decoderHeader = readSourceFile(QStringLiteral("decoder_context.h"));
  QVERIFY2(decoderHeader.contains(QStringLiteral("setAllowHardwareFrameMaterialization")) &&
               decoderHeader.contains(QStringLiteral("m_allowHardwareFrameMaterialization")) &&
               decoderContext.contains(QStringLiteral("!m_allowHardwareFrameMaterialization")),
           "CPU materialization for thumbnail/avatar decode must be a local "
           "DecoderContext option, not a global decode preference mutation");

  const QString renderExport = readSourceFile(QStringLiteral("render_export.cpp"));
  QVERIFY2(renderExport.contains(QStringLiteral("DecodePreference::HardwareZeroCopy")) &&
               renderExport.contains(QStringLiteral("JCUT_VULKAN_HW_DECODE_DISABLE")) &&
               !renderExport.contains(QStringLiteral("JCUT_VULKAN_HW_DECODE_EXPERIMENTAL")),
           "Vulkan export must prefer NVIDIA zero-copy decode by default and "
           "only fall back through an explicit disable switch");
}

void TestDirectVulkanHandoffPipelineContract::
    schedulingDiagnosticsExposeRequiredFields() {
  const QString profiling =
      readSourceFile(QStringLiteral("vulkan_preview_surface_profiling.cpp"));
  QVERIFY2(!profiling.isEmpty(),
           "vulkan_preview_surface_profiling.cpp must be readable");
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
      QStringLiteral("vulkan_visible_cpu_upload_fallback_enabled")};
  for (const QString &field : previewFields) {
    QVERIFY2(
        profiling.contains(field),
        qPrintable(QStringLiteral("preview perf diagnostics must expose %1")
                       .arg(field)));
  }

  const QString audio = readSourceFiles({
      QStringLiteral("audio_engine.h"),
      QStringLiteral("audio_engine.cpp"),
  });
  QVERIFY2(!audio.isEmpty(), "audio engine sources must be readable");
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
      QStringLiteral("stream_running")};
  for (const QString &field : audioFields) {
    QVERIFY2(
        audio.contains(field),
        qPrintable(
            QStringLiteral("audio diagnostics must expose %1").arg(field)));
  }

  const QString routes =
      readSourceFile(QStringLiteral("control_server_worker_routes.cpp"));
  QVERIFY2(!routes.isEmpty(),
           "control_server_worker_routes.cpp must be readable");
  QVERIFY2(routes.contains(QStringLiteral("/audio")),
           "REST API must expose audio loading/buffering state through /audio");
  QVERIFY2(routes.contains(QStringLiteral("/pipeline")),
           "REST API must expose preview decode/presentation scheduling state "
           "through /pipeline");
}

void TestDirectVulkanHandoffPipelineContract::
    pipelineDiagnosticsDefaultToCompactSnapshot() {
  const QString routes =
      readSourceFile(QStringLiteral("control_server_worker_routes.cpp"));
  QVERIFY2(!routes.isEmpty(),
           "control_server_worker_routes.cpp must be readable");
  QVERIFY2(
      routes.contains(
          QStringLiteral("queryBool(query, QStringLiteral(\"verbose\"))")),
      "/pipeline must require an explicit verbose query for rich debug data");
  QVERIFY2(routes.contains(QStringLiteral(
               "refreshPipelineSnapshotFromUi(m_uiInvokeTimeoutMs, verbose")),
           "/pipeline must pass the requested diagnostic detail level through "
           "the control boundary");

  const QString editorProfiling =
      readSourceFile(QStringLiteral("editor_profiling.cpp"));
  QVERIFY2(!editorProfiling.isEmpty(), "editor_profiling.cpp must be readable");
  QVERIFY2(editorProfiling.contains(
               QStringLiteral("m_preview->pipelineHealthSnapshot()")),
           "default /pipeline must use the compact health snapshot");
  QVERIFY2(editorProfiling.contains(
               QStringLiteral("m_preview->profilingSnapshot()")) &&
               editorProfiling.contains(QStringLiteral("if (verbose)")),
           "full profiling snapshot must remain explicit and verbose-only");

  const QString previewSurface =
      readSourceFile(QStringLiteral("preview_surface.h"));
  QVERIFY2(previewSurface.contains(QStringLiteral("pipelineHealthSnapshot")),
           "compact pipeline health must be a preview-surface contract");

  const QString vulkanProfiling =
      readSourceFile(QStringLiteral("vulkan_preview_surface_profiling.cpp"));
  QVERIFY2(
      vulkanProfiling.contains(QStringLiteral(
          "QJsonObject VulkanPreviewSurface::pipelineHealthSnapshot() const")),
      "Vulkan preview must implement the compact pipeline health snapshot");
  QVERIFY2(vulkanProfiling.contains(QStringLiteral(
               "pipelineStageHealthJson(livePipelineSnapshots())")),
           "compact /pipeline must expose named decode-to-preview stage state");
  QVERIFY2(vulkanProfiling.contains(QStringLiteral("decoder_diagnostics")),
           "compact /pipeline must expose decoder diagnostics needed to "
           "distinguish decode starvation");
  QVERIFY2(
      !vulkanProfiling
           .mid(
               vulkanProfiling.indexOf(QStringLiteral(
                   "QJsonObject VulkanPreviewSurface::pipelineHealthSnapshot() "
                   "const")),
               vulkanProfiling.indexOf(QStringLiteral(
                   "void VulkanPreviewSurface::resetProfilingStats()")) -
                   vulkanProfiling.indexOf(QStringLiteral(
                       "QJsonObject "
                       "VulkanPreviewSurface::pipelineHealthSnapshot() const")))
           .contains(QStringLiteral("currentSpeakerLabelDebugForState")),
      "compact pipeline polling must not perform speaker/transcript debug "
      "lookup");

  const QString vulkanSurface =
      readSourceFile(QStringLiteral("vulkan_preview_surface.cpp"));
  QVERIFY2(vulkanSurface.contains(QStringLiteral("13 Diagnostic Readback")) &&
               vulkanSurface.contains(QStringLiteral("diagnostic_disabled")),
           "pipeline stages must report diagnostic readback as opt-in, not as "
           "a hot-path render dependency");
}

void TestDirectVulkanHandoffPipelineContract::
    streamTimingDiagnosticsExposeClockDomains() {
  const QString routes =
      readSourceFile(QStringLiteral("control_server_worker_routes.cpp"));
  QVERIFY2(!routes.isEmpty(),
           "control_server_worker_routes.cpp must be readable");
  QVERIFY2(routes.contains(QStringLiteral("/diag/stream-timing")),
           "REST API must expose per-stream timing through /diag/stream-timing");
  QVERIFY2(routes.contains(QStringLiteral("m_streamTimingCallback")),
           "/diag/stream-timing must use the dedicated stream timing callback");

  const QString editorProfiling =
      readSourceFile(QStringLiteral("editor_profiling.cpp"));
  QVERIFY2(!editorProfiling.isEmpty(), "editor_profiling.cpp must be readable");
  const QStringList requiredFields{
      QStringLiteral("stream_timing"),
      QStringLiteral("snapshot_wall_ms"),
      QStringLiteral("master_timeline_sample"),
      QStringLiteral("session_start_wall_ms"),
      QStringLiteral("session_start_timeline_sample"),
      QStringLiteral("master_vs_wall_drift_samples"),
      QStringLiteral("projected_stream_start_wall_ms"),
      QStringLiteral("timeline_vs_projected_wall_drift_ms"),
      QStringLiteral("source_vs_projected_wall_drift_ms"),
      QStringLiteral("audio_feedback")};
  for (const QString& field : requiredFields) {
    QVERIFY2(editorProfiling.contains(field),
             qPrintable(QStringLiteral("stream timing diagnostics must expose %1")
                            .arg(field)));
  }
  QVERIFY2(editorProfiling.contains(
               QStringLiteral("sourceSampleForClipAtTimelineSample")),
           "stream timing must use the render-sync-aware source sample mapper");
  QVERIFY2(editorProfiling.contains(
               QStringLiteral("sourceFrameForClipAtTimelineSample")),
           "stream timing must use the same source frame mapper as preview");

  const QString playback = readSourceFile(QStringLiteral("editor_playback.cpp"));
  QVERIFY2(playback.contains(QStringLiteral("m_playbackSessionStartWallMs")) &&
               playback.contains(QStringLiteral("m_playbackSessionStartTimelineSample")),
           "playback start must latch a wall/sample anchor for stream timing");
}

void TestDirectVulkanHandoffPipelineContract::
    pitchPreservingAudioUsesExplicitSidecarGate() {
  const QString playback =
      readSourceFile(QStringLiteral("editor_playback.cpp"));
  QVERIFY2(!playback.isEmpty(), "editor_playback.cpp must be readable");
  QVERIFY2(
      playback.contains(QStringLiteral("needsPitchPreservingPlaybackAudio()")),
      "playback must centralize the decision to require pitch-preserving "
      "audio");
  QVERIFY2(playback.contains(QStringLiteral(
               "playbackAudioReadyForFrame(m_timeline->currentFrame())")),
           "playback startup must gate on the exact retimed audio needed at "
           "the current frame");
  QVERIFY2(
      playback.contains(QStringLiteral("requestPlaybackAudioWarmup(false)")),
      "missing retimed audio must enter a background warmup/generation path "
      "without stopping transport playback");
  QVERIFY2(
      playback.contains(QStringLiteral("Audio being generated")),
      "preview overlay must make retimed audio generation visible to the user");
  QVERIFY2(
      playback.contains(QStringLiteral("Loading re-timed audio")),
      "preview overlay must make retimed audio loading visible to the user");

  const QString audio = readSourceFiles({
      QStringLiteral("audio_engine.h"),
      QStringLiteral("audio_engine.cpp"),
  });
  QVERIFY2(!audio.isEmpty(), "audio engine sources must be readable");
  QVERIFY2(
      audio.contains(QStringLiteral(
          "snapshot[QStringLiteral(\"time_stretch_sidecar_only\")] = true")),
      "audio diagnostics must expose that pitch-preserving playback is "
      "sidecar-only");
  QVERIFY2(audio.contains(QStringLiteral("playbackAudioNeedsRetimingForFrame")),
           "audio engine must expose whether required retimed audio needs "
           "generation");
  QVERIFY2(!audio.contains(QStringLiteral("SOLA")),
           "sidecar-only pitch-preserving playback must not retain an implicit "
           "SOLA fallback path");
}

void TestDirectVulkanHandoffPipelineContract::
    noProxyHardwarePathIsPrimaryAndHoldsLateFrames() {
  const QString vulkanSurface =
      readSourceFile(QStringLiteral("vulkan_preview_surface.cpp"));
  QVERIFY2(!vulkanSurface.isEmpty(),
           "vulkan_preview_surface.cpp must be readable");

  QVERIFY2(vulkanSurface.contains(
               QStringLiteral("directVulkanDecodeClip(const TimelineClip& "
                              "clip, bool useProxyMedia)")),
           "direct Vulkan clip registration must make proxy use an explicit "
           "state, not an implicit fallback");
  QVERIFY2(vulkanSurface.contains(QStringLiteral("if (!useProxyMedia)")) &&
               vulkanSurface.contains(
                   QStringLiteral("directClip.useProxy = false")) &&
               vulkanSurface.contains(
                   QStringLiteral("directClip.proxyPath.clear()")),
           "no-proxy direct Vulkan playback must keep the original media path "
           "and only clear proxy state when proxy mode is disabled");
  QVERIFY2(
      !vulkanSurface.contains(QStringLiteral("stale_hardware_frame_rejected")),
      "late hardware frames must not be converted into missing black frames in "
      "the direct Vulkan presenter");
  QVERIFY2(vulkanSurface.contains(
               QStringLiteral("status.staleFrameRejected = selectedTooStale")),
           "late hardware frames must still be diagnosed as "
           "stale/current-frame failures");

  const QString testSource = readSourceFile(
      QStringLiteral("tests/test_no_proxy_hardware_playback_contract.cpp"));
  QVERIFY2(testSource.contains(QStringLiteral("JCUT_NO_PROXY_HARDWARE_VIDEO")),
           "the no-proxy hardware path must have an optional real-media "
           "headless fixture test");
  QVERIFY2(testSource.contains(QStringLiteral(
               "interactivePreviewMediaPathForClip(clip), fixturePath")),
           "the optional fixture must assert the no-proxy path resolves to "
           "original media");
  QVERIFY2(testSource.contains(QStringLiteral("frame.hasHardwareFrame()")),
           "the optional fixture must assert hardware payloads, not CPU proxy "
           "fallback");
}

void TestDirectVulkanHandoffPipelineContract::
    overlayWorkerKeepsNewestCoalescedRequest() {
  const QString header =
      readSourceFile(QStringLiteral("vulkan_preview_surface.h"));
  QVERIFY2(!header.isEmpty(), "vulkan_preview_surface.h must be readable");
  QVERIFY2(
      header.contains(QStringLiteral("m_queuedFacestreamOverlaySnapshotKey")),
      "overlay worker must retain the newest coalesced request key instead of "
      "dropping it");
  QVERIFY2(
      header.contains(QStringLiteral("m_queuedFacestreamOverlayRequestClips")),
      "overlay worker must retain the newest coalesced request payload");

  const QString source = readSourceFile(
      QStringLiteral("vulkan_preview_surface_facedetections.cpp"));
  QVERIFY2(!source.isEmpty(),
           "vulkan_preview_surface_facedetections.cpp must be readable");
  QVERIFY2(
      source.contains(QStringLiteral("startFacestreamOverlaySnapshotWorker(")),
      "overlay worker launch must be factored so queued follow-up requests can "
      "reuse it");
  QVERIFY2(
      source.contains(
          QStringLiteral("m_queuedFacestreamOverlaySnapshotKey = requestKey")),
      "newer overlay requests must replace the bounded queued request slot");
  QVERIFY2(
      source.contains(QStringLiteral("launchQueuedRequest();")),
      "overlay worker completion must launch the queued follow-up request");
  QVERIFY2(source.contains(
               QStringLiteral("facestreamOverlaySnapshotApplyDecision(")) &&
               source.contains(
                   QStringLiteral("++m_facedetectionsOverlayWorkerDropped")),
           "stale overlay worker result policy must be centralized and dropped "
           "results counted");

  const QString profiling =
      readSourceFile(QStringLiteral("vulkan_preview_surface_profiling.cpp"));
  QVERIFY2(!profiling.isEmpty(),
           "vulkan_preview_surface_profiling.cpp must be readable");
  QVERIFY2(
      profiling.contains(QStringLiteral("vulkan_overlay_worker_queued_key")),
      "perf diagnostics must expose the queued overlay worker request key");
  QVERIFY2(
      profiling.contains(
          QStringLiteral("vulkan_overlay_worker_queued_clip_count")),
      "perf diagnostics must expose the queued overlay worker request size");
}

void TestDirectVulkanHandoffPipelineContract::
    facestreamTrackBoxesAreNotBaselinePlaybackWork() {
  const QString header =
      readSourceFile(QStringLiteral("vulkan_preview_surface.h"));
  QVERIFY2(!header.isEmpty(), "vulkan_preview_surface.h must be readable");
  QVERIFY2(
      header.contains(QStringLiteral("bool m_showSpeakerTrackBoxes = false")),
      "Vulkan preview must not default FaceDetections/speaker-track boxes on; "
      "they can walk thousands of tracks during playback");

  const QString editor = readSourceFile(QStringLiteral("editor.cpp"));
  QVERIFY2(!editor.isEmpty(), "editor.cpp must be readable");
  QVERIFY2(editor.contains(QStringLiteral(
               "root.value(QStringLiteral(\"previewShowSpeakerTrackBoxes\"))."
               "toBool(false)")),
           "Project load fallback must keep FaceDetections/speaker-track boxes "
           "off unless explicitly saved enabled");

  const QString surface = readSourceFile(
      QStringLiteral("vulkan_preview_surface_facedetections.cpp"));
  QVERIFY2(!surface.isEmpty(),
           "vulkan_preview_surface_facedetections.cpp must be readable");
  QVERIFY2(surface.contains(QStringLiteral(
               "!m_showSpeakerTrackBoxes && "
               "!m_interaction.faceStreamAssignmentInteractionEnabled && "
               "!m_showRawDetections")),
           "FaceDetections overlay prep must be skipped entirely when boxes, "
           "raw detections, and assignment interaction are disabled");
}

void TestDirectVulkanHandoffPipelineContract::
    playbackFacestreamOverlaysDoNotColdLoadOnPresentationPath() {
  const QString source =
      readSourceFile(QStringLiteral("vulkan_preview_surface_facedetections.cpp"));
  QVERIFY2(!source.isEmpty(),
           "vulkan_preview_surface_facedetections.cpp must be readable");

  const int playbackStart =
      source.indexOf(QStringLiteral("if (m_interaction.playing)"));
  const int playbackReturn = source.indexOf(
      QStringLiteral("        return;\n    }\n\n"), playbackStart);
  const int pausedStart = source.indexOf(
      QStringLiteral("    for (const TimelineClip& clip : m_interaction.clips)"),
      playbackReturn);
  QVERIFY2(playbackStart >= 0 && playbackReturn > playbackStart &&
               pausedStart > playbackStart,
           "facestream overlay playback and paused branches must be visible "
           "for source inspection");

  const QString playbackBranch =
      source.mid(playbackStart, pausedStart - playbackStart);
  QVERIFY2(!playbackBranch.contains(QStringLiteral("loadFacestreamTracksForClip(")),
           "active playback must not synchronously hydrate cold facetrack "
           "buckets on the presentation path");
  QVERIFY2(playbackBranch.contains(QStringLiteral(
               "playback_cold_overlay_cache_missing_single_warmup")),
           "active playback cold facetrack misses must be reported as cache "
           "misses and preserve/clear overlays without blocking");
  QVERIFY2(playbackBranch.contains(QStringLiteral(
               "requestFacestreamOverlaySnapshotAsync")),
           "active playback may only prepare overlay primitives from already "
           "loaded facetrack buckets on the async overlay worker");
  QVERIFY2(playbackBranch.contains(QStringLiteral(
               "queueFacestreamOverlayCacheWarmup")),
           "active playback may schedule one cold facetrack cache warmup so "
           "face tracks can recover after an immediate playback start");
  QVERIFY2(playbackBranch.contains(QStringLiteral(
               "kFacestreamOverlayPlaybackWarmAheadFrames")),
           "active playback must keep future facetrack cache buckets warm so "
           "speaker tracks remain visible across bucket boundaries");
  QVERIFY2(!playbackBranch.contains(QStringLiteral(
               "reusedPlaybackCacheEntry")),
           "active playback must not apply arbitrary stale facetrack cache "
           "buckets when the exact bucket is cold");
  QVERIFY2(playbackBranch.contains(QStringLiteral(
               "previousPlaybackOverlayIsCloseEnough")),
           "active playback must preserve nearby previous overlays rather "
           "than blocking for a current-frame facetrack lookup");
  QVERIFY2(source.contains(QStringLiteral(
               "bool VulkanPreviewSurface::warmFacestreamOverlayLookahead")) &&
               source.contains(QStringLiteral(
                   "playback_overlay_warmup_loaded")) &&
               source.contains(QStringLiteral(
                   "playback_overlay_warmup_deferred")),
           "speaker/FaceDetections overlay warmup must be explicit and "
           "diagnosed separately from frame decode warmup");
  QVERIFY2(source.contains(QStringLiteral(
               "m_interaction.playing || (timeoutMs >= 0 && timer.elapsed() >= timeoutMs)")),
           "bounded pre-playback facetrack warmup must load cache buckets until "
           "the timeout is actually reached; otherwise face streams disappear "
           "as soon as playback starts");
  QVERIFY2(source.contains(QStringLiteral("m_lastFacestreamOverlayPlaybackWarmupMs")) &&
               source.contains(QStringLiteral("nowMs - m_lastFacestreamOverlayPlaybackWarmupMs < 1000")),
           "active playback facetrack cache recovery must be throttled instead "
           "of disabled, so boxes can recover at bucket boundaries");
  QVERIFY2(source.contains(QStringLiteral(
               "kMaxPreservedPlaybackOverlayDriftFrames =\n"
               "    kFacestreamOverlayInteractiveWindowFrames * 2")),
           "active playback overlay preservation must cover the prepared "
           "cache bucket window rather than only a couple of frames");
}

void TestDirectVulkanHandoffPipelineContract::
    rendererConsumesLatchedPreviewSnapshot() {
  const QString backend =
      readSourceFile(QStringLiteral("direct_vulkan_preview_window.cpp"));
  QVERIFY2(!backend.isEmpty(),
           "direct_vulkan_preview_window.cpp must be readable");

  const qsizetype liveStateIndex = backend.indexOf(QStringLiteral(
      "const PreviewInteractionState* liveState = m_owner->state();"));
  const qsizetype snapshotIndex =
      backend.indexOf(QStringLiteral("PreviewInteractionState renderSnapshot;"),
                      liveStateIndex);
  const qsizetype copyIndex = backend.indexOf(
      QStringLiteral("renderSnapshot = *liveState;"), snapshotIndex);
  const qsizetype stateAliasIndex =
      backend.indexOf(QStringLiteral("const PreviewInteractionState* state = "
                                     "liveState ? &renderSnapshot : nullptr;"),
                      copyIndex);
  QVERIFY2(liveStateIndex >= 0 && snapshotIndex > liveStateIndex &&
               copyIndex > snapshotIndex && stateAliasIndex > copyIndex,
           "direct Vulkan command recording must consume a stack-latched "
           "PreviewInteractionState snapshot");
}

void TestDirectVulkanHandoffPipelineContract::
    exportSpeakerLabelUsesFractionalMasterClockPosition() {
  const QString source =
      readSourceFile(QStringLiteral("offscreen_vulkan_renderer_backend.cpp"));
  QVERIFY2(!source.isEmpty(),
           "offscreen_vulkan_renderer_backend.cpp must be readable");
  QVERIFY2(source.contains(QStringLiteral("const RenderFrameClock &clock")),
           "export speaker label timing must accept the shared render frame "
           "clock used for the rendered output frame");
  const qsizetype labelIndex =
      source.indexOf(QStringLiteral("buildSpeakerLabelSpec"));
  QVERIFY2(labelIndex >= 0, "speaker label builder must exist");
  const qsizetype labelEndIndex =
      source.indexOf(QStringLiteral("private:"), labelIndex);
  QVERIFY2(labelEndIndex > labelIndex, "speaker label builder body must be bounded");
  const QString labelBody = source.mid(labelIndex, labelEndIndex - labelIndex);
  QVERIFY2(labelBody.contains(QStringLiteral("clipFrameMappingForClock")),
           "speaker label source-frame lookup must use the shared clip frame "
           "mapping object, not a floored timeline frame");
  QVERIFY2(labelBody.contains(QStringLiteral("clock.timelineSample")),
           "speaker label clip inclusion must use the shared clock timeline "
           "sample");
  QVERIFY2(!labelBody.contains(QStringLiteral("sourceFrameForClipAtTimelinePosition")),
           "speaker label source-frame lookup must not keep a parallel "
           "frame-position conversion path");
  QVERIFY2(labelBody.contains(QStringLiteral("transcriptOverlaySpeakerAtSourceFrame")),
           "export speaker labels must use the same padded transcript speaker "
           "resolver as preview overlays");
  QVERIFY2(labelBody.contains(QStringLiteral("mapping.transcriptFrame")),
           "export speaker labels must resolve transcript frames from the same "
           "clip mapping as video decode");
  QVERIFY2(!source.contains(QStringLiteral("speakerAtTranscriptSourceFrame")),
           "export must not keep a separate unpadded speaker resolver");
  QVERIFY2(source.contains(QStringLiteral("clip.sourceFrameSize.isValid()")) &&
               source.contains(QStringLiteral("? clip.sourceFrameSize")),
           "export video placement must prefer clip.sourceFrameSize before "
           "decoded payload size, matching direct preview transform geometry");
  QVERIFY2(source.contains(QStringLiteral("exportTextureNeedsYFlip = !layer.preferHardwareDirect")) &&
               source.contains(QStringLiteral("exportVideoLayerTranslationForSampledFace")),
           "export video layers must choose texture orientation by source type "
           "and compensate speaker-framing placement when a Y flip is required");
}

void TestDirectVulkanHandoffPipelineContract::
    speakerFramingUsesRenderSyncMarkersInPreviewAndExport() {
  const QString keyframes =
      readSourceFile(QStringLiteral("editor_shared_keyframes.cpp"));
  QVERIFY2(!keyframes.isEmpty(), "editor_shared_keyframes.cpp must be readable");
  QVERIFY2(keyframes.contains(QStringLiteral(
               "evaluateClipSpeakerFramingAtPosition(const TimelineClip& clip,\n"
               "                                                                     qreal timelineFramePosition,\n"
               "                                                                     const QVector<RenderSyncMarker>& markers")),
           "dynamic speaker framing must have a render-sync-aware position overload");
  QVERIFY2(keyframes.contains(QStringLiteral(
               "sourceFramePositionForClipAtTimelinePosition(clip, timelineFramePosition, markers)")),
           "speaker framing face-box lookup must resolve media source frame with "
           "the caller's render sync markers");
  QVERIFY2(keyframes.contains(QStringLiteral(
               "evaluateClipRenderTransformAtPosition(const TimelineClip& clip,\n"
               "                                                                      qreal timelineFramePosition,\n"
               "                                                                      const QVector<RenderSyncMarker>& markers")),
           "render transform evaluation must expose a marker-aware position overload");
  QVERIFY2(!keyframes.contains(QStringLiteral(
               "sourceFramePositionForClipAtTimelinePosition(clip, timelineFramePosition, {})")),
           "dynamic speaker framing must not use a marker-less source-frame path");

  const QString exportRenderer =
      readSourceFile(QStringLiteral("offscreen_vulkan_renderer_backend.cpp"));
  QVERIFY2(!exportRenderer.isEmpty(),
           "offscreen_vulkan_renderer_backend.cpp must be readable");
  QVERIFY2(exportRenderer.contains(QStringLiteral(
               "evaluateClipRenderTransformAtPosition(\n"
               "            clip,\n"
               "            static_cast<qreal>(timelineFrame),\n"
               "            request.renderSyncMarkers,\n"
               "            request.outputSize,\n"
               "            &transformDiagnostics)")),
           "export must pass request.renderSyncMarkers into render transform "
           "evaluation so speaker framing targets the same face box as preview");
  QVERIFY2(exportRenderer.contains(QStringLiteral(
               "exportFaceTransformDiagnostics")),
           "export must expose face-box/transform diagnostics from the same "
           "speaker-framing transform path");

  const QString preview =
      readSourceFile(QStringLiteral("vulkan_preview_surface.cpp"));
  QVERIFY2(!preview.isEmpty(), "vulkan_preview_surface.cpp must be readable");
  QVERIFY2(preview.contains(QStringLiteral(
               "evaluateClipRenderTransformAtPosition(\n"
               "            clip,\n"
               "            m_interaction.currentFramePosition,\n"
               "            m_interaction.renderSyncMarkers,\n"
               "            m_interaction.outputSize)")),
           "preview must pass its interaction render sync markers into the same "
           "render transform evaluation path");
}

void TestDirectVulkanHandoffPipelineContract::
    speakerFramingAndExportUseFractionalFitGeometry() {
  const QString keyframes =
      readSourceFile(QStringLiteral("editor_shared_keyframes.cpp"));
  QVERIFY2(!keyframes.isEmpty(), "editor_shared_keyframes.cpp must be readable");
  QVERIFY2(keyframes.contains(QStringLiteral(
               "QRectF fitRectForSourceInOutput")),
           "speaker framing must solve against fractional fitted bounds, "
           "matching direct preview geometry");
  QVERIFY2(keyframes.contains(QStringLiteral("previewFitRectToBoundsF")),
           "speaker framing must share the preview's floating-point fit helper");
  QVERIFY2(!keyframes.contains(QStringLiteral(
               "const QRect fittedRect = fitRectForSourceInOutput")),
           "speaker framing must not quantize fitted bounds before computing "
           "face-box translation");

  const QString renderDecode = readSourceFile(QStringLiteral("render_decode.cpp"));
  QVERIFY2(!renderDecode.isEmpty(), "render_decode.cpp must be readable");
  QVERIFY2(renderDecode.contains(QStringLiteral("QRectF fitRectF")),
           "render placement must expose a fractional fit helper");
  QVERIFY2(renderDecode.contains(QStringLiteral("previewFitRectToBoundsF")),
           "render placement must use the same floating-point fit helper as preview");

  const QString exportRenderer =
      readSourceFile(QStringLiteral("offscreen_vulkan_renderer_backend.cpp"));
  QVERIFY2(!exportRenderer.isEmpty(),
           "offscreen_vulkan_renderer_backend.cpp must be readable");
  QVERIFY2(exportRenderer.contains(QStringLiteral(
               "const QRectF fitted = fitRectF(sourceSize, request.outputSize);")),
           "export video layer placement must keep the same fractional fitted "
           "bounds as preview");
  QVERIFY2(!exportRenderer.contains(QStringLiteral(
               "const QRect fitted = fitRect(sourceSize, request.outputSize);")),
           "export video layer placement must not round fitted bounds before "
           "applying speaker-framing translation");
}

void TestDirectVulkanHandoffPipelineContract::
    contiguousTranscriptSectionsCanHoldMultipleTracks() {
  const QString tracks = readSourceFile(QStringLiteral("tracks.cpp"));
  QVERIFY2(!tracks.isEmpty(), "tracks.cpp must be readable");
  QVERIFY2(tracks.contains(QStringLiteral("sectionTrackEntriesWithTrack")),
           "contiguous section assignment must merge clicked tracks into a "
           "section-level track list");
  QVERIFY2(tracks.contains(QStringLiteral("assignTrackToContiguousSections")),
           "contiguous section assignment must support applying one clicked "
           "track to every matching contiguous transcript section row");
  QVERIFY2(tracks.contains(QStringLiteral("sectionRowWithTrackEntries")),
           "contiguous section rows must persist a tracks array while keeping "
           "legacy primary track fields");
  QVERIFY2(tracks.contains(QStringLiteral("row[QStringLiteral(\"tracks\")] = entries")),
           "contiguous section rows must write the full tracks array");
  QVERIFY2(tracks.contains(QStringLiteral("targetSectionKeys")) &&
               tracks.contains(QStringLiteral("existingSectionRows")),
           "assigning another track to matching contiguous transcript sections "
           "must preserve and extend existing rows");
  QVERIFY2(!tracks.contains(QStringLiteral("if (!sameTrack)")),
           "assigning a track to one contiguous transcript section must not "
           "evict that same track from other contiguous sections");
  const qsizetype sectionAssignIndex = tracks.indexOf(
      QStringLiteral("if (contiguousMode) {"));
  QVERIFY2(sectionAssignIndex >= 0,
           "face-box left-click assignment must have an explicit contiguous "
           "section path");
  const qsizetype speakerAssignIndex = tracks.indexOf(
      QStringLiteral("const bool assigned = assignTrackToSpeaker"), sectionAssignIndex);
  QVERIFY2(speakerAssignIndex > sectionAssignIndex,
           "contiguous section assignment body must be bounded before the "
           "speaker-level assignment fallback");
  const QString sectionAssignBody =
      tracks.mid(sectionAssignIndex, speakerAssignIndex - sectionAssignIndex);
  QVERIFY2(sectionAssignBody.contains(QStringLiteral("speakerSectionRowsAtFrame(sectionSpeakerId, transcriptFrame)")) &&
               sectionAssignBody.contains(QStringLiteral("assignTrackToContiguousSections(")),
           "left-clicking a face track in contiguous transcript mode must "
           "resolve every target section from the clicked track transcript time");
  QVERIFY2(!sectionAssignBody.contains(QStringLiteral("speakerSectionsTable->currentRow()")),
           "contiguous section left-click assignment must not use the table's "
           "currently selected row as the target section");

  const QString speakers = readSourceFile(QStringLiteral("speakers_tab.cpp"));
  QVERIFY2(!speakers.isEmpty(), "speakers_tab.cpp must be readable");
  QVERIFY2(speakers.contains(QStringLiteral("sectionTrackEntriesFromAssignment")),
           "contiguous transcript table must read section assignments as a "
           "track list");
  QVERIFY2(speakers.contains(QStringLiteral("sectionTrackIdStringsFromAssignment")),
           "contiguous transcript table row roles must expose every assigned "
           "track, not only the primary track");
  QVERIFY2(speakers.contains(QStringLiteral("previewAssignedFaceTrackIdsForSpeakerAtFrame")) &&
               speakers.contains(QStringLiteral("contiguousTranscriptSectionModeActive()")) &&
               speakers.contains(QStringLiteral("section_track_map")) &&
               speakers.contains(QStringLiteral("resolvedCurrentTrackIdsForSpeaker")),
           "preview track selection sync must use contiguous section-track "
           "mapping in section mode and speaker-track identity mapping outside it");
  QVERIFY2(speakers.contains(QStringLiteral("speakerSectionMinimumWords")) &&
               speakers.contains(QStringLiteral("sectionAssignmentWordCount")) &&
               speakers.contains(QStringLiteral("currentRow.wordCount >= minimumWords")),
           "contiguous transcript section rows must be filtered by the shared "
           "minimum word-count control");
  QVERIFY2(speakers.contains(QStringLiteral("SpeakerSectionTrackIdsRole")) &&
               speakers.contains(QStringLiteral("trackIdStrings")),
           "section export rows must pass mapped track ids through to the "
           "batch export path");
  QVERIFY2(tracks.contains(QStringLiteral("pushPreviewAssignedFaceTrackIdsForSpeakerAtFrame")) &&
               tracks.contains(QStringLiteral("transcriptFrameForClipSourceFrame")) &&
               tracks.contains(QStringLiteral("\"word_count\"")),
           "contiguous section assignment must push the clicked track to the "
           "preview immediately in the transcript timing domain and persist "
           "section word counts for runtime filtering");

  const QString keyframes =
      readSourceFile(QStringLiteral("editor_shared_keyframes.cpp"));
  QVERIFY2(!keyframes.isEmpty(), "editor_shared_keyframes.cpp must be readable");
  QVERIFY2(keyframes.contains(QStringLiteral("sectionTrackEntriesForRuntime")),
           "runtime speaker framing must expand a section assignment into all "
           "assigned tracks");
  QVERIFY2(keyframes.contains(QStringLiteral("trackIds->insert(trackId)")) &&
               keyframes.contains(QStringLiteral("streamIds->insert(streamId)")),
           "runtime speaker framing must collect every assigned track/stream "
           "for the active contiguous section");
  QVERIFY2(keyframes.contains(QStringLiteral("sectionMappingActive")) &&
               keyframes.contains(QStringLiteral("sectionMappingActive && !matchedSectionAssignment")) &&
               keyframes.contains(QStringLiteral("!sectionMappingActive &&")) &&
               keyframes.contains(QStringLiteral("if (sectionMap.isEmpty())")),
           "runtime speaker framing must use either contiguous section-track "
           "mapping or speaker-track identity mapping for a clip, not both");
  QVERIFY2(keyframes.contains(QStringLiteral("sectionAssignmentWordCountForRuntime")) &&
               keyframes.contains(QStringLiteral("clip.speakerSectionMinimumWords")),
           "runtime speaker framing must skip contiguous transcript sections "
           "below the clip's minimum word-count requirement");

  const QString inspector = readSourceFile(QStringLiteral("inspector_pane.cpp"));
  QVERIFY2(!inspector.isEmpty(), "inspector_pane.cpp must be readable");
  QVERIFY2(inspector.contains(QStringLiteral("m_speakerSectionMinimumWordsSpin")) &&
               inspector.contains(QStringLiteral("Min words ")),
           "Speakers section mode must expose a minimum-word-count control "
           "above the section table");

  const QString serialization = readSourceFile(QStringLiteral("clip_serialization.cpp"));
  QVERIFY2(!serialization.isEmpty(), "clip_serialization.cpp must be readable");
  QVERIFY2(serialization.contains(QStringLiteral("speakerSectionMinimumWords")),
           "minimum section word count must persist with the clip so preview "
           "and export render paths see the same filter");

  const QString renderTools = readSourceFile(QStringLiteral("editor_render_tools.cpp"));
  QVERIFY2(!renderTools.isEmpty(), "editor_render_tools.cpp must be readable");
  const qsizetype batchExportIndex = renderTools.indexOf(
      QStringLiteral("void EditorWindow::exportVideoForSpeakerSectionsOnSelectedClip"));
  QVERIFY2(batchExportIndex >= 0,
           "batch section export implementation must exist");
  const QString batchExportBody = renderTools.mid(batchExportIndex);
  QVERIFY2(batchExportBody.contains(QStringLiteral("coalescedAdjacentSpeakerSections")) &&
               batchExportBody.contains(QStringLiteral("speakerTrackSectionTitle")) &&
               renderTools.contains(QStringLiteral("normalizedSectionTrackIds")) &&
               renderTools.contains(QStringLiteral("trackIds")),
           "Export Sections must combine adjacent same-speaker rows and name "
           "outputs from speaker name plus mapped track numbers");
  QVERIFY2(batchExportBody.contains(QStringLiteral("deterministicExportPath")) &&
               batchExportBody.contains(QStringLiteral("QFileInfo::exists(outputPath)")) &&
               batchExportBody.contains(QStringLiteral("request.suppressCompletionDialog = true")) &&
               batchExportBody.contains(QStringLiteral("renderTimelineFromOutputRequest(request, false, &bulkControls)")),
           "Export Sections must run unattended after the initial setup and "
           "skip deterministic output paths that already exist");
  QVERIFY2(!batchExportBody.contains(QStringLiteral("runAiAction")) &&
               !batchExportBody.contains(QStringLiteral("name_transcript_section")),
           "Export Sections batch naming must be deterministic and must not "
           "consult AI");

  const QString routes =
      readSourceFile(QStringLiteral("control_server_worker_routes.cpp"));
  QVERIFY2(!routes.isEmpty(),
           "control_server_worker_routes.cpp must be readable");
  QVERIFY2(routes.contains(QStringLiteral("row[QStringLiteral(\"tracks\")] = tracks")) &&
               routes.contains(QStringLiteral("row[QStringLiteral(\"track_ids\")] = trackIds")),
           "REST track-map diagnostics must expose the full contiguous-section "
           "track list");
}

void TestDirectVulkanHandoffPipelineContract::
    trackAssignmentDoesNotCreateFaceBoxKeyframes() {
  const QString tracks = readSourceFile(QStringLiteral("tracks.cpp"));
  QVERIFY2(!tracks.isEmpty(), "tracks.cpp must be readable");
  const qsizetype functionIndex = tracks.indexOf(
      QStringLiteral("bool SpeakersTab::applyPreviewFaceBoxSpeakerFramingTrackSelection"));
  QVERIFY2(functionIndex >= 0,
           "face-box assignment framing selection helper must exist");
  const qsizetype functionEnd =
      tracks.indexOf(QStringLiteral("bool SpeakersTab::deassignTrackFromSpeaker"), functionIndex);
  QVERIFY2(functionEnd > functionIndex,
           "face-box assignment framing selection helper body must be bounded");
  const QString body = tracks.mid(functionIndex, functionEnd - functionIndex);
  QVERIFY2(!body.contains(QStringLiteral("speakerFramingTargetKeyframes.push_back")),
           "assigning a face track must not create a new speaker-framing "
           "target/face-box keyframe");
  QVERIFY2(!body.contains(QStringLiteral("speakerFramingEnabledKeyframes.push_back")),
           "assigning a face track must not create a new speaker-framing "
           "enabled keyframe");
  QVERIFY2(body.contains(QStringLiteral("target.title")) &&
               body.contains(QStringLiteral("Speaker framing target from assigned face track")),
           "existing speaker-framing target keyframes updated by assignment "
           "must receive an identifying title");

  const QString clipSerialization =
      readSourceFile(QStringLiteral("clip_serialization.cpp"));
  QVERIFY2(!clipSerialization.isEmpty(),
           "clip_serialization.cpp must be readable");
  QVERIFY2(clipSerialization.contains(QStringLiteral("keyframeObj[QStringLiteral(\"title\")]")) &&
               clipSerialization.contains(QStringLiteral("keyframe.title = keyframeObj.value(QStringLiteral(\"title\"))")),
           "transform-style keyframe titles must round-trip through project "
           "serialization");

  const QString assignmentService =
      readSourceFile(QStringLiteral("speaker_track_assignment_service.cpp"));
  QVERIFY2(assignmentService.contains(QStringLiteral("Speaker track assignment anchor T%1")),
           "speaker assignment anchors must be titled for future diagnostics");

  QVERIFY2(tracks.contains(QStringLiteral("Contiguous section assignment anchor T%1")),
           "contiguous section assignment anchors must be titled for future "
           "diagnostics");
  QVERIFY2(tracks.contains(QStringLiteral("contiguous_section_rotation")) &&
               tracks.contains(QStringLiteral("sectionRow[QStringLiteral(\"tracks\")] = QJsonArray()")),
           "per-row section rotation must be persistable before any face track "
           "is assigned to that contiguous transcript section");
  QVERIFY2(!tracks.contains(QStringLiteral("row.remove(QStringLiteral(\"rotation\"))")),
           "normalizing an empty-track contiguous section row must not discard "
           "its independent rotation");
}

void TestDirectVulkanHandoffPipelineContract::
    speechFilterBlendUsesPrecomputedSampleRanges() {
  const QString header = readSourceFile(QStringLiteral("audio_engine.h"));
  const QString source = readSourceFile(QStringLiteral("audio_engine.cpp"));
  QVERIFY2(!header.isEmpty(), "audio_engine.h must be readable");
  QVERIFY2(!source.isEmpty(), "audio_engine.cpp must be readable");

  QVERIFY2(header.contains(QStringLiteral("struct SpeechSampleRange")),
           "audio speech filtering must have a precomputed sample-domain range type");
  QVERIFY2(header.contains(QStringLiteral("QVector<SpeechSampleRange> speechSampleRanges")),
           "audio mix context must carry precomputed sample-domain speech ranges");
  QVERIFY2(source.contains(QStringLiteral("context.speechSampleRanges.reserve")) &&
               source.contains(QStringLiteral("SpeechSampleRange{startSample, endSampleExclusive}")),
           "audio mix loop must precompute speech-filter sample ranges once per chunk");
  QVERIFY2(source.contains(QStringLiteral("std::upper_bound(")) &&
               source.contains(QStringLiteral("const SpeechSampleRange &range")),
           "speech-filter blend lookup must use ordered sample ranges instead of scanning all export ranges");

  const int fnStart = source.indexOf(QStringLiteral(
      "AudioEngine::SpeechRangeBlend AudioEngine::calculateSpeechRangeBlend"));
  const int nextFn = source.indexOf(QStringLiteral(
      "float AudioEngine::calculateClipCrossfadeGain"), fnStart);
  QVERIFY2(fnStart >= 0 && nextFn > fnStart,
           "calculateSpeechRangeBlend body must be visible for inspection");
  const QString blendBody = source.mid(fnStart, nextFn - fnStart);
  QVERIFY2(!blendBody.contains(QStringLiteral("timelineFrameToSamples(")) &&
               !blendBody.contains(QStringLiteral("frameToSamples(")),
           "per-sample speech blend calculation must not convert transcript frames to samples");
}

void TestDirectVulkanHandoffPipelineContract::
    vulkanTextShaderUsesVulkanFramebufferYConvention() {
  const QString shader =
      readSourceFile(QStringLiteral("shaders/vulkan/text.vert"));
  QVERIFY2(!shader.isEmpty(), "text.vert must be readable");

  const qsizetype topLeftPos =
      shader.indexOf(QStringLiteral("pos = vec2(-1.0, -1.0);"));
  const qsizetype topLeftUv =
      shader.indexOf(QStringLiteral("unitUv = vec2(0.0, 0.0);"), topLeftPos);
  QVERIFY2(topLeftPos >= 0 && topLeftUv > topLeftPos,
           "top-left Vulkan framebuffer vertex must sample the top-left glyph "
           "atlas UV");

  const qsizetype bottomRightPos =
      shader.indexOf(QStringLiteral("pos = vec2(1.0, 1.0);"));
  const qsizetype bottomRightUv = shader.indexOf(
      QStringLiteral("unitUv = vec2(1.0, 1.0);"), bottomRightPos);
  QVERIFY2(bottomRightPos >= 0 && bottomRightUv > bottomRightPos,
           "bottom-right Vulkan framebuffer vertex must sample the "
           "bottom-right glyph atlas UV");

  QVERIFY2(!shader.contains(QStringLiteral(
               "pos = vec2(-1.0, -1.0);\n        unitUv = vec2(0.0, 1.0);")),
           "text shader must not use legacy Y-flipped glyph UVs in the "
           "Vulkan presenter");
}

QTEST_MAIN(TestDirectVulkanHandoffPipelineContract)
#include "test_direct_vulkan_handoff_pipeline_contract.moc"
