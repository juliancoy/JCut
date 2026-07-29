#include <QtTest/QtTest>

#include "../direct_vulkan_media_handoff_plan.h"

#include <QFile>
#include <QImage>
#include <QString>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

class TestDirectVulkanHandoffPipelineContract : public QObject {
  Q_OBJECT

private slots:
  void directPreviewUsesExtractedPipelineBeforeRenderPass();
  void directPreviewUsesNativePresentContract();
  void presentationMissTelemetryUsesActualVisibleFrames();
  void exactVfrMaskIdentityIsObservable();
  void directPreviewRecordsTextureUploadsBeforeRenderPass();
  void directPreviewRecordsGpuHandoffIntoFrameCommandBuffer();
  void hardwareDirectExportBuffersAreRetiredBeforeReplacement();
  void uploadStagingBuffersAreRetiredBeforeReplacement();
  void sampledImagesAreRetiredBeforeReplacement();
  void graphicsPipelinesDeclareDisabledDepthStencilState();
  void directPreviewTransitionsAuxiliarySampledImagesBeforeDraw();
  void directPreviewUsesPerClipHandoffDescriptors();
  void mediaOwnerPlanDeduplicatesHiddenParentChildren();
  void mediaOwnerPayloadReuseFailsClosed();
  void maskChildrenUseExplicitOwners();
  void descriptorUpdatesFollowAcquiredSwapchainOwnership();
  void maskChildrenFailClosedWithoutAMatte();
  void maskSidecarsPrefetchWithPlaybackWindow();
  void directPreviewRequiresHardwarePayloadsFromCache();
  void handoffPipelineRejectsCpuOnlyFrames();
  void strictDisplayabilityDoesNotAcceptCpuFallback();
  void directPreviewDisablesCpuAndQtTextOverlayFallbacks();
  void directPreviewDrawsAudioOnlyTranscriptOverlaysAfterVideoLoop();
  void exportUsesSharedTranscriptSourceResolution();
  void visibleDecodePriorityUsesTimelineDomain();
  void schedulingDiagnosticsExposeRequiredFields();
  void streamTimingDiagnosticsExposeClockDomains();
  void streamTimingDiagnosticsUseEffectiveProxyState();
  void timelineUseProxyMenuControlsEffectiveProxyState();
  void timelineContextMenuControlsClipRenderVisibility();
  void pipelineDiagnosticsDefaultToCompactSnapshot();
  void playbackTelemetryUsesCanonicalAtomicsWithoutUiInvocation();
  void transportControlDoesNotCollectUiProfiles();
  void latestPresentedFrameImageExposesCpuPresentedFrame();
  void playbackReadinessRequiresExactFrames();
  void playbackPipelineUsesTransportSampleDomain();
  void gradingPreviewControlRestoresSelectedTrackState();
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
  void maskMorphControlsUseWideSliderInputs();
  void maskDropShadowAndFalloffReachPreviewAndExport();
  void startupRestoresSpeechFilterRouting();
  void playbackRangesUseMutationDrivenCache();
  void speechFilterPassthroughModePersistsAsPassThroughState();
  void speechFilterFadeParametersOnlyShowWhenRelevant();
  void effectsExposeSpeechFilterSynchronizedMotion();
  void speechFilterFrameCrossfadeIsVisibleInDirectPreview();
  void transcriptTimingEditsInvertDisplayPadding();
  void speechFilterBlendUsesPrecomputedSampleRanges();
  void vulkanTextShaderUsesVulkanFramebufferYConvention();
  void exportPreviewUsesGpuDoubleBufferOnDedicatedSurface();
  void exportRunsOffGuiThreadWhileDedicatedSurfacePresents();
  void renderSynchronizationWaitsAreBoundedAndDiagnosable();
  void incrementalExportCheckpointsAndLosslesslyRemuxes();
  void outputTabClearsOnlyResolvedIncrementalRenderCacheRoot();
  void exportCompositionNeverPublishesPartialLayers();
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

void TestDirectVulkanHandoffPipelineContract::maskChildrenUseExplicitOwners() {
  const QString statusHeader =
      readSourceFile(QStringLiteral("preview_interaction_state.h"));
  const QString surface =
      readSourceFile(QStringLiteral("vulkan_preview_surface.cpp"));
  const QString preview =
      readSourceFile(QStringLiteral("direct_vulkan_preview_window.cpp"));
  const QString exportBackend =
      readSourceFile(QStringLiteral("offscreen_vulkan_renderer_backend.cpp"));

  QVERIFY2(statusHeader.contains(QStringLiteral("QString mediaOwnerClipId")) &&
               statusHeader.contains(QStringLiteral("QString timingOwnerClipId")) &&
               statusHeader.contains(QStringLiteral("QString effectsOwnerClipId")) &&
               statusHeader.contains(QStringLiteral("QString matteOwnerClipId")),
           "preview status must name every mask-child owner explicitly");
  QVERIFY2(surface.contains(QStringLiteral("markerStatus.mediaOwnerClipId = sourceId")) &&
               surface.contains(QStringLiteral("markerStatus.timingOwnerClipId = sourceId")) &&
               surface.contains(QStringLiteral("markerStatus.effectsOwnerClipId = clip.id")) &&
               surface.contains(QStringLiteral("markerStatus.matteOwnerClipId = clip.id")),
           "mask children must use parent media/timing and child effects/matte");
  QVERIFY2(surface.contains(QStringLiteral(
               "maskChildStatusFromParentMediaAndTiming(parentStatus)")) &&
               !surface.contains(QStringLiteral(
                   "VulkanPreviewClipFrameStatus markerStatus = statusByClipId.value(sourceId)")),
           "mask children must copy only parent media/timing state, not the parent's evaluated effects");
  QVERIFY2(preview.contains(QStringLiteral(
               "status->timingOwnerClipId == status->mediaOwnerClipId")) &&
               preview.contains(QStringLiteral(
                   "status->effectsOwnerClipId == clip.id")) &&
               preview.contains(QStringLiteral(
                   "status->matteOwnerClipId == clip.id")),
           "direct preview must fail closed when mask ownership is inconsistent");
  QVERIFY2(exportBackend.contains(QStringLiteral(
               "const TimelineClip &mediaOwner = timingSource")) &&
               exportBackend.contains(QStringLiteral(
                   "const TimelineClip &timingOwner = timingSource")) &&
               exportBackend.contains(QStringLiteral(
                   "const TimelineClip &effectsOwner = clip")) &&
               exportBackend.contains(QStringLiteral(
                   "const TimelineClip &matteOwner = clip")),
           "export must apply the same parent/parent/child/child ownership rule");
}

void TestDirectVulkanHandoffPipelineContract::directPreviewUsesNativePresentContract() {
  const QString source =
      readSourceFile(QStringLiteral("direct_vulkan_preview_window.cpp"));
  const QString surface =
      readSourceFile(QStringLiteral("vulkan_preview_surface.cpp"));
  QVERIFY2(!source.isEmpty(),
           "direct_vulkan_preview_window.cpp must be readable");
  QVERIFY2(!surface.isEmpty(),
           "vulkan_preview_surface.cpp must be readable");
  const qsizetype frameReady = source.lastIndexOf(QStringLiteral("m_window->frameReady();"));
  const qsizetype rendererEnd = source.indexOf(
      QStringLiteral("\n}\n\nvoid DirectVulkanPreviewRenderer::physicalDeviceLost"), frameReady);
  const QString afterFrameReady =
      frameReady >= 0 && rendererEnd > frameReady
          ? source.mid(frameReady, rendererEnd - frameReady)
          : QString();
  const qsizetype eventStart =
      source.indexOf(QStringLiteral("bool event(QEvent* event) override"));
  const qsizetype eventEnd =
      source.indexOf(QStringLiteral("void updatePreviewCursor"), eventStart);
  const QString eventBody =
      eventStart >= 0 && eventEnd > eventStart
          ? source.mid(eventStart, eventEnd - eventStart)
          : QString();
  const qsizetype startNextFrame = source.indexOf(
      QStringLiteral("void DirectVulkanPreviewRenderer::startNextFrame()"));
  const qsizetype firstCommandBuffer = source.indexOf(
      QStringLiteral("VkCommandBuffer cb ="), startNextFrame);
  const QString unavailableRendererPath =
      startNextFrame >= 0 && firstCommandBuffer > startNextFrame
          ? source.mid(startNextFrame, firstCommandBuffer - startNextFrame)
          : QString();
  QVERIFY2(!source.contains(QStringLiteral("requestUpdate();")) &&
               source.contains(QStringLiteral("class DirectVulkanPreviewWindow final : public QWindow")) &&
               !source.contains(QStringLiteral("#include <QVulkanWindow>")) &&
               !source.contains(QStringLiteral("class DirectVulkanPreviewRenderer final : public QVulkanWindowRenderer")) &&
               !source.contains(QStringLiteral("m_updateDeliveryQueued")) &&
               !source.contains(QStringLiteral("kStalePreviewUpdateMs")) &&
               source.contains(QStringLiteral("if (!isExposed())")) &&
               source.contains(QStringLiteral("m_updateDirty = true")) &&
               source.contains(QStringLiteral("m_frameInProgress")) &&
               !source.contains(QStringLiteral("bool m_updatePending = false;")) &&
               !source.contains(QStringLiteral("qint64 m_acceptedUpdateRequestMs = -1;")) &&
               source.contains(QStringLiteral("QVulkanInstance::surfaceForWindow(this)")) &&
               source.contains(QStringLiteral("vkCreateSwapchainKHR")) &&
               source.contains(QStringLiteral("vkAcquireNextImageKHR")) &&
               source.contains(QStringLiteral("vkQueuePresentKHR")) &&
               source.contains(QStringLiteral("presentAboutToBeQueued(this)")) &&
               source.contains(QStringLiteral("presentQueued(this)")) &&
               source.contains(QStringLiteral("renderNow();")) &&
               source.contains(QStringLiteral("m_owner->beginPreviewFrame()")) &&
               unavailableRendererPath.contains(
                   QStringLiteral("m_window->frameReady()")) &&
               unavailableRendererPath.contains(
                   QStringLiteral("m_owner->markPreviewUpdateDelivered()")) &&
               eventBody.contains(QStringLiteral("previewUpdateEventsDelivered.fetch_add(")) &&
               eventBody.contains(QStringLiteral("QWindow::event(event)")) &&
               !eventBody.contains(QStringLiteral("if (m_updatePending)")) &&
               frameReady >= 0 && rendererEnd > frameReady &&
               !afterFrameReady.contains(QStringLiteral("schedulePreviewUpdate")) &&
               surface.contains(QStringLiteral("setCurrentPlaybackSample")) &&
               surface.contains(QStringLiteral("requestNativeUpdate();")),
           "preview rendering must use a native Vulkan swapchain presenter on "
           "a plain QWindow host, with timeline playback ticks driving direct "
           "render/present work instead of QVulkanWindow frame callbacks");
}

void TestDirectVulkanHandoffPipelineContract::
    presentationMissTelemetryUsesActualVisibleFrames() {
  const QString preview =
      readSourceFile(QStringLiteral("direct_vulkan_preview_window.cpp"));
  const QString pipeline =
      readSourceFile(QStringLiteral("playback_frame_pipeline.cpp"));
  const QString profiling =
      readSourceFile(QStringLiteral("vulkan_preview_surface_profiling.cpp"));
  QVERIFY2(!preview.isEmpty() && !pipeline.isEmpty() && !profiling.isEmpty(),
           "presentation telemetry sources must be readable");

  const qsizetype getterStart = pipeline.indexOf(
      QStringLiteral("FrameHandle PlaybackFramePipeline::getPresentationFrame"));
  const qsizetype getterEnd = pipeline.indexOf(
      QStringLiteral("bool PlaybackFramePipeline::isFrameBuffered"), getterStart);
  const QString getterBody =
      getterStart >= 0 && getterEnd > getterStart
          ? pipeline.mid(getterStart, getterEnd - getterStart)
          : QString();
  QVERIFY2(!getterBody.isEmpty() &&
               !getterBody.contains(QStringLiteral("fetch_add")) &&
               !getterBody.contains(QStringLiteral("droppedPresentation")),
           "getPresentationFrame must remain a read-only selection query");

  QVERIFY2(
      preview.contains(QStringLiteral(
          "m_presentationMissTracker.recordPresentedFrame(samples)")) &&
          preview.contains(QStringLiteral(
              "presentationStatusRequiresDraw(")) &&
          preview.contains(QStringLiteral(
              "presentedFrameForDrawOutcome(")) &&
          preview.contains(QStringLiteral(
              "submittedClipIds.insert(status->clipId)")) &&
          preview.contains(QStringLiteral(
              "submittedCrossfadeClipIds.insert(status->clipId)")) &&
          preview.contains(QStringLiteral(
              "status.maskClipSource")) &&
          preview.contains(QStringLiteral(
              "status.frameCrossfadeRequestedSourceFrame")) &&
          preview.contains(QStringLiteral(
              "state, &submittedClipIds, &submittedCrossfadeClipIds")),
      "misses must be recorded once at the actual visible presentation "
      "boundary from submitted primary/crossfade draws, with mask children "
      "tracked independently from their shared media owner");
  const qsizetype telemetryStart =
      preview.indexOf(QStringLiteral("void markPresented("));
  const qsizetype presentedCounterStart =
      preview.indexOf(QStringLiteral("const qint64 nowMs"), telemetryStart);
  const QString telemetryBody =
      telemetryStart >= 0 && presentedCounterStart > telemetryStart
          ? preview.mid(telemetryStart, presentedCounterStart - telemetryStart)
          : QString();
  QVERIFY2(
      !telemetryBody.isEmpty() &&
          !telemetryBody.contains(QStringLiteral("!status.hasFrame")),
      "a visible requested frame with no presented payload is itself one "
      "unique presentation miss");
  QVERIFY2(
      !profiling.contains(QStringLiteral("droppedPresentationFrameCount()")),
      "surface profiling must not replace presenter telemetry with repeated "
      "frame-selection lookups");
  QVERIFY2(
      preview.contains(QStringLiteral("void resetProfilingAnchors()")) &&
          preview.contains(QStringLiteral("m_lastPresentMs = 0")) &&
          preview.contains(QStringLiteral(
              "m_presentationMissTracker.reset()")),
      "profile reset must clear both the miss tracker and presentation timing "
      "anchor so the next frame cannot inherit a pre-reset interval");
}

void TestDirectVulkanHandoffPipelineContract::
    exactVfrMaskIdentityIsObservable() {
  const QString presenter =
      readSourceFile(QStringLiteral("direct_vulkan_preview_presenter.cpp"));
  const QString profiling =
      readSourceFile(QStringLiteral("vulkan_preview_surface_profiling.cpp"));
  const QString routes =
      readSourceFile(QStringLiteral("control_server_worker_routes.cpp"));
  QVERIFY2(!presenter.isEmpty() && !profiling.isEmpty() && !routes.isEmpty(),
           "VFR presentation diagnostic sources must be readable");

  const QString primaryField = QStringLiteral(
      "presented_source_video_stream_best_effort_timestamp");
  const QString primaryAvailableField = QStringLiteral(
      "presented_source_video_stream_best_effort_timestamp_available");
  const QString crossfadeField = QStringLiteral(
      "frame_crossfade_presented_source_video_stream_best_effort_timestamp");
  const QString crossfadeAvailableField = QStringLiteral(
      "frame_crossfade_presented_source_video_stream_best_effort_timestamp_available");
  const QString activeField = QStringLiteral(
      "active_presented_source_video_stream_best_effort_timestamp");
  const QString activeAvailableField = QStringLiteral(
      "active_presented_source_video_stream_best_effort_timestamp_available");

  QVERIFY2(presenter.contains(primaryField) &&
               presenter.contains(primaryAvailableField) &&
               presenter.contains(crossfadeField) &&
               presenter.contains(crossfadeAvailableField) &&
               presenter.contains(QStringLiteral(
                   "status.frame.sourcePresentationTimestamp()")) &&
               presenter.contains(QStringLiteral(
                   "status.frameCrossfadeFrame.sourcePresentationTimestamp()")),
           "verbose per-layer diagnostics must expose the actual primary and "
           "crossfade FrameHandle identities in the source video stream "
           "best-effort-timestamp domain");
  QVERIFY2(profiling.count(activeField) >= 2 &&
               profiling.count(activeAvailableField) >= 2 &&
               profiling.contains(QStringLiteral(
                   "static_cast<qint64>(status.frame.sourcePresentationTimestamp())")),
           "compact and verbose preview snapshots must expose the active "
           "FrameHandle timestamp as qint64");
  QVERIFY2(routes.count(activeField) >= 2 &&
               routes.count(activeAvailableField) >= 2 &&
               routes.contains(QStringLiteral(
                   ".toInteger(std::numeric_limits<qint64>::min())")),
           "playback sync and playback diagnostics must forward the active "
           "timestamp without lossy floating-point conversion");
}

void TestDirectVulkanHandoffPipelineContract::
    directPreviewUsesExtractedPipelineBeforeRenderPass() {
  const QString source =
      readSourceFile(QStringLiteral("direct_vulkan_preview_window.cpp"));
  QVERIFY2(!source.isEmpty(),
           "direct_vulkan_preview_window.cpp must be readable");

  const qsizetype recordIndex =
      source.indexOf(QStringLiteral("mediaOwnerResources->pipeline->record("));
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
    maskChildrenFailClosedWithoutAMatte() {
  const QString source =
      readSourceFile(QStringLiteral("direct_vulkan_preview_window.cpp"));
  QVERIFY2(!source.isEmpty(),
           "direct_vulkan_preview_window.cpp must be readable");
  QVERIFY2(source.contains(QStringLiteral(
               "if (status->maskClipSource && !maskReady)")),
           "a mask child without a ready matte must not fall back to a full-frame draw");
  QVERIFY2(source.contains(QStringLiteral("mask_texture_unavailable")),
           "missing mask textures must be exposed in renderer diagnostics");
  QVERIFY2(!source.contains(QStringLiteral(
               "maskUploadResults.value(mediaOwnerId, false)")),
           "mask children may reuse parent media, but must upload their own matte");
  QVERIFY2(source.contains(QStringLiteral(
               "const QString handoffResourceId = status.clipId")),
           "mask children must own descriptor resources independently of parent media");
  QVERIFY2(source.contains(QStringLiteral(
               "setSampledImage(ownerResult.imageView")),
           "mask children must reuse only the parent's decoded image view");
  const QString previewSurface =
      readSourceFile(QStringLiteral("vulkan_preview_surface.cpp"));
  QVERIFY2(previewSurface.contains(QStringLiteral(
               "status.drawSuppressed = activeAsMediaProvider && !activeAsVisibleLayer")),
           "a hidden parent retained only as a Mask Matte media provider must "
           "not composite as a full-frame preview layer");
  QVERIFY2(previewSurface.contains(QStringLiteral(
               "clipVisualPlaybackEnabled(clip, m_interaction.tracks) ||\n"
               "                    maskMatteSourceIds.contains(clip.id)")),
           "the hidden parent status must survive draw ordering so the Vulkan "
           "handoff plan can use it as the canonical media provider");
  QVERIFY2(previewSurface.contains(QStringLiteral(
               "sourceParent->clipRole == ClipRole::Media")) &&
               previewSurface.contains(QStringLiteral(
                   "sourceParent->mediaType == ClipMediaType::Video")),
           "preview must reject orphaned mattes and non-video/non-media parents");
  QVERIFY2(previewSurface.contains(QStringLiteral("clip.maskEnabled")) &&
               previewSurface.contains(QStringLiteral(
                   "markerStatus.maskShowOnly = clip.maskShowOnly")),
           "child enablement and mask-only display must survive the preview handoff");
  const qsizetype markerLoopBegin = previewSurface.indexOf(
      QStringLiteral("orderedStatuses.reserve"));
  const qsizetype markerLoopPredicateEnd = previewSurface.indexOf(
      QStringLiteral("const QString sourceId"), markerLoopBegin);
  const QString markerLoopPredicate =
      markerLoopBegin >= 0 && markerLoopPredicateEnd > markerLoopBegin
          ? previewSurface.mid(markerLoopBegin,
                               markerLoopPredicateEnd - markerLoopBegin)
          : QString();
  QVERIFY2(markerLoopPredicate.contains(QStringLiteral(
               "clipVisualPlaybackEnabled(clip, m_interaction.tracks)")),
           "preview must not clone or composite a Mask Matte status when its "
           "generated child track or clip visibility is disabled");
  QVERIFY2(previewSurface.contains(QStringLiteral(
               "status.correctionPolygons = effects.correctionPolygons")) &&
               previewSurface.contains(QStringLiteral(
                   "markerStatus.correctionPolygons = effects.correctionPolygons")) &&
               !previewSurface.contains(QStringLiteral(
                   "applyCorrectionPolygonsToMaskBuffer")) &&
               !previewSurface.contains(QStringLiteral(
                   "qtImageFromCoreBuffer(markerStatus.maskBuffer)")),
           "Mask Matte correction polygons must remain GPU metadata beside "
           "the shared Gray8 buffer in direct Vulkan preview");
  const QString previewState =
      readSourceFile(QStringLiteral("preview_interaction_state.h"));
  QVERIFY2(previewState.contains(QStringLiteral("frameCrossfadeMaskBuffer")) &&
               previewState.contains(QStringLiteral(
                   "frameCrossfadeMaskTextureEnabled")) &&
               previewSurface.contains(QStringLiteral(
                   "markerStatus.frameCrossfadeMaskBuffer")) &&
               previewSurface.contains(QStringLiteral(
                   "markerStatus.frameCrossfadeFrame")),
           "masked speech-boundary crossfades must resolve the matte for the "
           "secondary actual presented FrameHandle");
  QVERIFY2(source.contains(QStringLiteral(
               "frameCrossfadeMaskUploadResults.insert")) &&
               source.contains(QStringLiteral(
                   "status.frameCrossfadeMaskBuffer")) &&
               source.contains(QStringLiteral("frameCrossfadeMaskReady")) &&
               source.contains(QStringLiteral(
                   "!status->maskTextureEnabled")),
           "the secondary crossfade descriptor must own its matching matte "
           "and every masked clip must fail closed when that upload is unavailable");
  const QString trackPreviewSources = readSourceFiles({
      QStringLiteral("editor_inspector_bindings.cpp"),
      QStringLiteral("vulkan_preview_surface.cpp"),
      QStringLiteral("project_state.cpp")});
  QVERIFY2(trackPreviewSources.contains(QStringLiteral(
               "track.gradingPreviewEnabled = checked")),
           "the grading Preview control must update the selected clip's track");
  QVERIFY2(trackPreviewSources.contains(QStringLiteral(
               "gradingPreviewEnabledForTrack")),
           "preview rendering must evaluate grading visibility per track");
  QVERIFY2(trackPreviewSources.contains(QStringLiteral(
               "trackObj[QStringLiteral(\"gradingPreviewEnabled\")]")),
           "per-track grading Preview state must persist with the timeline");
}

void TestDirectVulkanHandoffPipelineContract::
    maskSidecarsPrefetchWithPlaybackWindow() {
  const QString surface =
      readSourceFile(QStringLiteral("vulkan_preview_surface.cpp"));
  const QString effects =
      readSourceFile(QStringLiteral("editor_shared_effects.cpp"));
  const QString imageDecoder =
      readSourceFile(QStringLiteral("core/image_file_decoder.cpp"));
  const QString vulkanResources =
      readSourceFile(QStringLiteral("vulkan_resources.cpp"));
  QVERIFY2(!surface.isEmpty() && !effects.isEmpty() &&
               !imageDecoder.isEmpty() && !vulkanResources.isEmpty(),
           "mask playback sources must be readable");
  QVERIFY2(surface.contains(QStringLiteral(
               "prefetchMaskBuffersForPlayback();")) &&
               surface.contains(QStringLiteral(
                   "effectivePlaybackLookaheadFrames()")) &&
               surface.contains(QStringLiteral(
                   "prefetchRenderableClipMaskBuffersForClock(")) &&
               effects.contains(QStringLiteral(
                   "prefetchRenderableClipMaskBuffersForClock(")) &&
               effects.contains(QStringLiteral(
                   "prefetchClipMaskBuffers(clip, qMax<int64_t>(0, mapping.sourceFrame))")),
           "mask-sidecar decoding must follow the configured video playback "
           "window through the shared preview/export prefetch path instead of "
           "starting only after a frame is presented");
  QVERIFY2(surface.contains(QStringLiteral(
               "&m_maskPrefetchWindowKeys")) &&
               effects.contains(QStringLiteral(
                   "previousWindowKeys->contains(requestKey)")),
           "the sliding mask window must not repeat filesystem work for frames "
           "that remain inside the playback lookahead");
  QVERIFY2(
      surface.contains(
          QStringLiteral("kMaximumMaskLookaheadFrames = 16")) &&
          surface.contains(
              QStringLiteral(
                  "qMin(effectivePlaybackLookaheadFrames(), "
                  "kMaximumMaskLookaheadFrames)")),
      "single-channel masks must spend their reduced memory footprint on at "
      "least half a second of decode lead instead of retaining the old "
      "RGBA-era four-frame cap");
  QVERIFY2(
      surface.contains(
          QStringLiteral("transitionMaskWindowFrames")) &&
          surface.contains(
              QStringLiteral("upcomingRangeStart + offset")),
      "the incoming mask window must be decoded before a discontinuous range "
      "transition, not one mask at a time inside the crossfade");
  QVERIFY2(effects.contains(QStringLiteral(
               "std::thread::hardware_concurrency()")) &&
               effects.contains(QStringLiteral("workers_.emplace_back")),
           "mask decoding must scale across a bounded architecture-neutral "
           "worker pool rather than serialize every active matte");
  QVERIFY2(
      effects.contains(QStringLiteral("decodeImageFileGray(corePath)")) &&
          imageDecoder.contains(
              QStringLiteral("PixelFormat::Gray8")) &&
          vulkanResources.contains(
              QStringLiteral("VK_FORMAT_R8_UNORM")) &&
          vulkanResources.contains(
              QStringLiteral("image.format != jcut::core::PixelFormat::Gray8")),
      "raw mask cache and Vulkan staging must stay single-channel instead of "
      "expanding every full-resolution matte to RGBA");
  QVERIFY2(!surface.contains(QStringLiteral("m_lastMaskBuffer")),
           "preview must not pair a stale matte with a newer presented frame");
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
    directPreviewRecordsGpuHandoffIntoFrameCommandBuffer() {
  const QString previewSource =
      readSourceFile(QStringLiteral("direct_vulkan_preview_window.cpp"));
  const QString pipelineSource =
      readSourceFile(QStringLiteral("direct_vulkan_frame_handoff_pipeline.cpp"));
  QVERIFY2(!previewSource.isEmpty(),
           "direct_vulkan_preview_window.cpp must be readable");
  QVERIFY2(!pipelineSource.isEmpty(),
           "direct_vulkan_frame_handoff_pipeline.cpp must be readable");

  QVERIFY2(
      pipelineSource.contains(
          QStringLiteral("handoff->recordHardwareFrameUpload(commandBuffer, status.frame")),
      "direct preview handoff must record hardware-frame upload work into "
      "the native preview frame command buffer");
  QVERIFY2(
      pipelineSource.contains(
          QStringLiteral("handoff->recordImportedFrameCopy(commandBuffer, offscreenFrame")),
      "direct preview external Vulkan handoff must record import copies into "
      "the native preview frame command buffer");
  QVERIFY2(
      !pipelineSource.contains(QStringLiteral("m_handoff->uploadFrame(")) &&
          !pipelineSource.contains(QStringLiteral("m_handoff->importOffscreenFrame(")),
      "direct preview must not submit and wait on a separate handoff command "
      "buffer during presentation");
  QVERIFY2(
      pipelineSource.contains(QStringLiteral("handoffForFrameSlot(uint32_t frameSlot)")) &&
          pipelineSource.contains(QStringLiteral("frameSlot) % m_handoffs.size()")),
      "direct preview must key independent handoff resources by swapchain "
      "frame slot so a slot is reused only after Qt/Vulkan reacquires that "
      "swapchain image");
  QVERIFY2(
      previewSource.contains(QStringLiteral("currentSwapChainImageIndex()")) &&
          previewSource.contains(QStringLiteral("swapchainImageIndex")),
      "direct preview must pass the native presenter's current swapchain image index "
      "into the handoff resource selection");
  QVERIFY2(
      pipelineSource.contains(QStringLiteral("!status.externalVulkanFrame && !status.frame.hasHardwareFrame()")),
      "direct preview must disable VulkanDetectorFrameHandoff CPU upload "
      "fallback; visible frames require hardware/external GPU payloads");
}

void TestDirectVulkanHandoffPipelineContract::
    hardwareDirectExportBuffersAreRetiredBeforeReplacement() {
  const QString source =
      readSourceFile(QStringLiteral("vulkan_detector_frame_handoff.cpp"));
  const QString header =
      readSourceFile(QStringLiteral("vulkan_detector_frame_handoff.h"));
  QVERIFY2(!source.isEmpty(),
           "vulkan_detector_frame_handoff.cpp must be readable");
  QVERIFY2(!header.isEmpty(),
           "vulkan_detector_frame_handoff.h must be readable");

  QVERIFY2(header.contains(QStringLiteral("RetiredCudaExportBuffer")) &&
               header.contains(QStringLiteral("m_retiredCudaExportBuffers")),
           "hardware-direct CUDA export buffers must have a retired-resource "
           "list for buffers that may still be referenced by frame command "
           "buffers");
  QVERIFY2(source.contains(QStringLiteral("retireCudaExportBuffer(")) &&
               source.contains(QStringLiteral("destroyRetiredCudaExportBuffers()")),
           "hardware-direct CUDA export buffers must be retired and drained "
           "through explicit helpers");
  QVERIFY2(source.contains(QStringLiteral("vkDeviceWaitIdle(m_context.device);")) &&
               source.contains(QStringLiteral("destroyRetiredCudaExportBuffers();")),
           "retired CUDA export buffers must be destroyed only after device "
           "work is quiesced during handoff release");

  const qsizetype ensureStart =
      source.indexOf(QStringLiteral("bool VulkanDetectorFrameHandoff::ensureCudaExportBuffer"));
  const qsizetype nextFunction =
      source.indexOf(QStringLiteral("bool VulkanDetectorFrameHandoff::createNv12ConversionPipeline"),
                     ensureStart);
  QVERIFY2(ensureStart >= 0 && nextFunction > ensureStart,
           "ensureCudaExportBuffer function body must be discoverable");
  const QString ensureBody = source.mid(ensureStart, nextFunction - ensureStart);
  QVERIFY2(!ensureBody.contains(QStringLiteral("destroyBuffer(buffer, memory, &m_resourceStats")),
           "ensureCudaExportBuffer must not immediately destroy an export "
           "buffer that may still be referenced by an in-flight presentation "
           "command buffer");
  QVERIFY2(source.count(QStringLiteral("retireCudaExportBuffer(m_cudaExportBuffer")) >= 2 &&
               source.count(QStringLiteral("retireCudaExportBuffer(m_cudaExportUvBuffer")) >= 2,
           "both hardware-direct upload paths must retire Y/RGBA and UV "
           "export buffers before replacement");
}

void TestDirectVulkanHandoffPipelineContract::
    uploadStagingBuffersAreRetiredBeforeReplacement() {
  const QString source = readSourceFile(QStringLiteral("vulkan_resources.cpp"));
  const QString header = readSourceFile(QStringLiteral("vulkan_resources.h"));
  QVERIFY2(!source.isEmpty(), "vulkan_resources.cpp must be readable");
  QVERIFY2(!header.isEmpty(), "vulkan_resources.h must be readable");

  QVERIFY2(header.contains(QStringLiteral("RetiredStagingBuffer")) &&
               header.contains(QStringLiteral("m_retiredStagingBuffers")),
           "Vulkan upload staging buffers must have a retired-resource list "
           "for buffers referenced by submitted frame command buffers");
  QVERIFY2(header.contains(QStringLiteral("reserveStagingUpload")) &&
               header.contains(QStringLiteral("writeStagingUpload")) &&
               header.contains(QStringLiteral("beginFrameUploads")) &&
               header.contains(QStringLiteral("StagingUploadRing")) &&
               header.contains(QStringLiteral("resetAllocation()")) &&
               header.contains(QStringLiteral("writeOffset")),
           "VulkanResources must track per-upload staging offsets so later "
           "LUT uploads cannot overwrite earlier texture or mask uploads in "
           "the same recorded command buffer");
  QVERIFY2(source.contains(QStringLiteral("m_retiredStagingBuffers.push_back(retired)")) &&
               source.contains(QStringLiteral("m_retiredStagingBuffers.clear()")),
           "Vulkan upload staging buffers must be retired on growth and "
           "drained on resource destruction");
  QVERIFY2(source.contains(QStringLiteral("bool VulkanResources::reserveStagingUpload")) &&
               source.contains(QStringLiteral("bool VulkanResources::writeStagingUpload")) &&
               source.contains(QStringLiteral("bool VulkanResources::beginFrameUploads")) &&
               source.contains(QStringLiteral("checkedAdd")) &&
               source.contains(QStringLiteral("checkedMul")) &&
               source.contains(QStringLiteral("alignUp")) &&
               source.count(QStringLiteral("region.bufferOffset = stagingOffset")) >= 5 &&
               source.count(QStringLiteral("writeStagingUpload(")) >= 6,
           "every direct-preview staging upload path must reserve a distinct "
           "buffer slice and copy from that slice");
  QVERIFY2(source.contains(QStringLiteral("m_stagingRing.frameSlotBytes = std::max(kTextureBytes, kCurveLutBytes)")) &&
               source.contains(QStringLiteral("initialStagingBytes")),
           "initial direct-preview staging allocation must be sized for the "
           "per-frame slot layout instead of reallocating on the first real "
           "frame upload");
  QVERIFY2(source.contains(QStringLiteral("m_stagingRing.resetAllocation()")) &&
               !source.contains(QStringLiteral("m_stagingRing.reset();\n    }\n\n    VkBufferCreateInfo bufferInfo")),
           "staging buffer growth must preserve the active swapchain frame "
           "slot; only full resource teardown should reset the whole ring");
  QVERIFY2(source.contains(QStringLiteral("return false;")) &&
               !source.contains(QStringLiteral("m_stagingRing.writeOffset = 0;\n    }\n    return true;")),
           "beginFrameUploads must fail explicitly if the frame-slot base "
           "cannot be represented; it must not silently fall back to offset 0");
  QVERIFY2(!source.contains(QStringLiteral("vkMapMemory(m_device, m_stagingMemory, 0, bytes")) &&
               !source.contains(QStringLiteral("vkMapMemory(m_device, m_stagingMemory, 0, kCurveLutBytes")) &&
               !source.contains(QStringLiteral("vkMapMemory(m_device, m_stagingMemory, 0, kTextureBytes")),
           "direct-preview staging uploads must not keep remapping offset 0 "
           "while multiple copies are recorded into one frame command buffer");
  QVERIFY2(source.contains(QStringLiteral("vkDeviceWaitIdle(m_device);")) &&
               source.contains(QStringLiteral("for (RetiredStagingBuffer& retired")),
           "retired staging buffers must be destroyed only after device work "
           "is quiesced");

  const qsizetype ensureStart =
      source.indexOf(QStringLiteral("bool VulkanResources::ensureStagingCapacity"));
  const qsizetype nextFunction =
      source.indexOf(QStringLiteral("uint32_t VulkanResources::findMemoryType"),
                     ensureStart);
  QVERIFY2(ensureStart >= 0 && nextFunction > ensureStart,
           "ensureStagingCapacity function body must be discoverable");
  const QString ensureBody = source.mid(ensureStart, nextFunction - ensureStart);
  const qsizetype createInfoIndex =
      ensureBody.indexOf(QStringLiteral("VkBufferCreateInfo bufferInfo"));
  QVERIFY2(createInfoIndex > 0,
           "ensureStagingCapacity must create a replacement buffer after "
           "handling the previous staging buffer");
  const QString preCreateBody = ensureBody.left(createInfoIndex);
  QVERIFY2(!preCreateBody.contains(
               QStringLiteral("vkDestroyBuffer(m_device, m_stagingBuffer, nullptr);")),
           "ensureStagingCapacity must not destroy the previous staging "
           "buffer while earlier presentation command buffers may still copy "
           "from it");
}

void TestDirectVulkanHandoffPipelineContract::
    sampledImagesAreRetiredBeforeReplacement() {
  const QString source = readSourceFile(QStringLiteral("vulkan_resources.cpp"));
  const QString header = readSourceFile(QStringLiteral("vulkan_resources.h"));
  QVERIFY2(!source.isEmpty(), "vulkan_resources.cpp must be readable");
  QVERIFY2(!header.isEmpty(), "vulkan_resources.h must be readable");

  QVERIFY2(header.contains(QStringLiteral("RetiredImageResource")) &&
               header.contains(QStringLiteral("m_retiredImageResources")),
           "resized sampled images must have a retired-resource list for "
           "image views/images/memory referenced by submitted command buffers");
  QVERIFY2(source.contains(QStringLiteral("retireTextureImage()")) &&
               source.contains(QStringLiteral("retireMaskImage(m_maskImage")) &&
               source.contains(QStringLiteral("retireMaskImage(m_maskRawImage")) &&
               source.contains(QStringLiteral("m_retiredImageResources.clear()")),
           "texture and mask resize paths must retire old sampled images and "
           "drain them on resource destruction");

  const qsizetype createTextureStart =
      source.indexOf(QStringLiteral("bool VulkanResources::createTextureImage"));
  const qsizetype createMaskStart =
      source.indexOf(QStringLiteral("bool VulkanResources::createMaskImage"),
                     createTextureStart);
  QVERIFY2(createTextureStart >= 0 && createMaskStart > createTextureStart,
           "createTextureImage body must be discoverable");
  const QString createTextureBody =
      source.mid(createTextureStart, createMaskStart - createTextureStart);
  const qsizetype firstCreateCall =
      createTextureBody.indexOf(QStringLiteral("vkCreateImage("));
  QVERIFY2(firstCreateCall > 0, "createTextureImage must create a replacement image");
  const QString preCreateTextureBody = createTextureBody.left(firstCreateCall);
  QVERIFY2(preCreateTextureBody.contains(QStringLiteral("retireTextureImage();")) &&
               !preCreateTextureBody.contains(QStringLiteral("destroyTextureImage();")),
           "createTextureImage must retire the previous sampled texture "
           "instead of destroying it before replacement");
}

void TestDirectVulkanHandoffPipelineContract::
    graphicsPipelinesDeclareDisabledDepthStencilState() {
  const QStringList paths{
      QStringLiteral("vulkan_pipeline.cpp"),
      QStringLiteral("vulkan_text_renderer.cpp"),
      QStringLiteral("vulkan_audio_tab.cpp"),
      QStringLiteral("offscreen_vulkan_renderer_backend.cpp")};
  for (const QString& path : paths) {
    const QString source = readSourceFile(path);
    QVERIFY2(!source.isEmpty(),
             qPrintable(QStringLiteral("%1 must be readable").arg(path)));
    QVERIFY2(source.contains(QStringLiteral("VkPipelineDepthStencilStateCreateInfo depthStencil")) &&
                 source.contains(QStringLiteral("depthStencil.depthTestEnable = VK_FALSE")) &&
                 source.contains(QStringLiteral("depthStencil.stencilTestEnable = VK_FALSE")) &&
                 source.contains(QStringLiteral("pipelineInfo.pDepthStencilState = &depthStencil")),
             qPrintable(QStringLiteral(
                            "%1 must provide an explicit disabled "
                            "depth/stencil state for render passes with "
                            "depth/stencil attachments")
                            .arg(path)));
  }
}

void TestDirectVulkanHandoffPipelineContract::
    directPreviewTransitionsAuxiliarySampledImagesBeforeDraw() {
  const QString resources = readSourceFile(QStringLiteral("vulkan_resources.cpp"));
  const QString preview = readSourceFile(QStringLiteral("direct_vulkan_preview_window.cpp"));
  QVERIFY2(!resources.isEmpty(), "vulkan_resources.cpp must be readable");
  QVERIFY2(!preview.isEmpty(), "direct_vulkan_preview_window.cpp must be readable");

  QVERIFY2(resources.contains(QStringLiteral("ensureAuxiliaryImagesReadable")) &&
               resources.contains(QStringLiteral("transitionIfUndefined(m_curveLutImage, m_curveLutLayout)")) &&
               resources.contains(QStringLiteral("transitionIfUndefined(m_maskImage, m_maskLayout)")) &&
               resources.contains(QStringLiteral("transitionIfUndefined(m_maskCurveLutImage, m_maskCurveLutLayout)")),
           "VulkanResources must transition default auxiliary sampled images "
           "to shader-read layout before descriptor sets using them are drawn");

  const qsizetype ensureIndex =
      preview.indexOf(QStringLiteral("ensureAuxiliaryImagesReadable(cb)"));
  const qsizetype beginRenderPassIndex =
      preview.indexOf(QStringLiteral("vkCmdBeginRenderPass"));
  QVERIFY2(ensureIndex >= 0 && beginRenderPassIndex > ensureIndex,
           "direct preview must make auxiliary sampled images readable before "
           "starting the render pass that draws descriptor sets");
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
               QStringLiteral("ensureClipHandoffResources(handoffResourceId)")),
           "direct preview must resolve handoff resources by clip identity");
  const QString handoffPlan =
      readSourceFile(QStringLiteral("direct_vulkan_media_handoff_plan.h"));
  QVERIFY2(!handoffPlan.isEmpty(),
           "direct_vulkan_media_handoff_plan.h must be readable");
  QVERIFY2(backend.contains(QStringLiteral("const QString handoffResourceId = status.clipId")) &&
               handoffPlan.contains(QStringLiteral("status.mediaOwnerClipId.trimmed()")) &&
               backend.contains(QStringLiteral("handoffResources->resources->setSampledImage(ownerResult.imageView")),
           "virtual mask children must reuse the parent's sampled image through a child-owned descriptor bundle");
  const qsizetype prepareOwnerBegin = backend.indexOf(
      QStringLiteral("const auto prepareBaseMediaOwner"));
  const qsizetype prepareOwnerEnd = backend.indexOf(
      QStringLiteral("// Upload/import each decoded media payload exactly once"),
      prepareOwnerBegin);
  const QString prepareOwnerBody =
      prepareOwnerBegin >= 0 && prepareOwnerEnd > prepareOwnerBegin
          ? backend.mid(prepareOwnerBegin, prepareOwnerEnd - prepareOwnerBegin)
          : QString();
  QVERIFY2(prepareOwnerBody.contains(QStringLiteral(
               "mediaOwnerResources->resources->uploadImageTexture")) &&
               prepareOwnerBody.contains(QStringLiteral(
                   "mediaOwnerResources->pipeline->record")),
           "the single media-owner preparation path must cover both CPU uploads "
           "and hardware/external Vulkan handoff");
  QVERIFY2(backend.contains(QStringLiteral(
               "mediaOwnerHandoffResults.insert(entry.mediaOwnerClipId, ownerResult)")) &&
               backend.contains(QStringLiteral(
                   "mediaOwnerPayloadMatches(providerStatus, status)")),
           "children must reuse one canonical owner result and fail closed when "
           "their cloned payload identity diverges");
  const qsizetype childReuseBegin = backend.indexOf(QStringLiteral(
      "const DirectVulkanFrameHandoffPipeline::Result ownerResult ="));
  const qsizetype childReuseEnd = backend.indexOf(
      QStringLiteral("const auto prepareAuxiliaryFrame"), childReuseBegin);
  const QString childReuseBody =
      childReuseBegin >= 0 && childReuseEnd > childReuseBegin
          ? backend.mid(childReuseBegin, childReuseEnd - childReuseBegin)
          : QString();
  QVERIFY2(childReuseBody.contains(QStringLiteral("setSampledImage")) &&
               !childReuseBody.contains(QStringLiteral("pipeline->record")) &&
               !childReuseBody.contains(QStringLiteral("uploadImageTexture")),
           "a Mask Matte consumer must bind its owner's prepared image and "
           "must never upload or import that primary FrameHandle independently");
  QVERIFY2(backend.contains(QStringLiteral("const QString ownerSecondaryKey")) &&
               backend.contains(QStringLiteral(
                   "mediaOwnerHandoffResults.value(ownerSecondaryKey)")) &&
               backend.contains(QStringLiteral(
                   "secondaryHandoffResources->resources->setSampledImage")),
           "frame-crossfade payloads inherited by mask children must also be "
           "handed off once per media owner and rebound through child descriptors");
  QVERIFY2(backend.contains(QStringLiteral("if (!status.maskClipSource && !curveLut.isEmpty())")),
           "mask children must not overwrite the parent's normal grading LUT");
  QVERIFY2(backend.contains(QStringLiteral("if (!handoffResources->resources->beginFrameUploads(")),
           "every clip-owned descriptor bundle must select the acquired frame upload slot");
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
    mediaOwnerPlanDeduplicatesHiddenParentChildren() {
  VulkanPreviewClipFrameStatus hiddenParent;
  hiddenParent.clipId = QStringLiteral("source");
  hiddenParent.mediaOwnerClipId = hiddenParent.clipId;
  hiddenParent.active = true;
  hiddenParent.drawSuppressed = true;

  VulkanPreviewClipFrameStatus alphaChild;
  alphaChild.clipId = QStringLiteral("alpha");
  alphaChild.mediaOwnerClipId = hiddenParent.clipId;
  alphaChild.active = true;

  VulkanPreviewClipFrameStatus personChild = alphaChild;
  personChild.clipId = QStringLiteral("person");

  VulkanPreviewClipFrameStatus unavailableChild = alphaChild;
  unavailableChild.clipId = QStringLiteral("unavailable");
  unavailableChild.drawSuppressed = true;

  VulkanPreviewClipFrameStatus independentClip;
  independentClip.clipId = QStringLiteral("other-source");
  independentClip.mediaOwnerClipId = independentClip.clipId;
  independentClip.active = true;

  const QVector<VulkanPreviewClipFrameStatus> statuses = {
      hiddenParent, alphaChild, personChild, unavailableChild, independentClip};
  const auto plan =
      jcut::direct_vulkan_preview::mediaOwnerHandoffPlan(statuses);

  QCOMPARE(plan.size(), 2);
  QCOMPARE(plan.at(0).mediaOwnerClipId, QStringLiteral("source"));
  QCOMPARE(plan.at(0).providerStatusIndex, 0);
  QCOMPARE(plan.at(0).consumerStatusIndices.size(), 2);
  QCOMPARE(plan.at(0).consumerStatusIndices.at(0), 1);
  QCOMPARE(plan.at(0).consumerStatusIndices.at(1), 2);
  QCOMPARE(plan.at(1).mediaOwnerClipId, QStringLiteral("other-source"));
  QCOMPARE(plan.at(1).providerStatusIndex, 4);

  const auto parentOmittedPlan =
      jcut::direct_vulkan_preview::mediaOwnerHandoffPlan({alphaChild, personChild});
  QCOMPARE(parentOmittedPlan.size(), 1);
  QCOMPARE(parentOmittedPlan.at(0).mediaOwnerClipId, QStringLiteral("source"));
  QCOMPARE(parentOmittedPlan.at(0).providerStatusIndex, 0);
  QCOMPARE(parentOmittedPlan.at(0).consumerStatusIndices.size(), 2);
}

void TestDirectVulkanHandoffPipelineContract::
    mediaOwnerPayloadReuseFailsClosed() {
  const QImage image(8, 8, QImage::Format_RGBA8888);
  VulkanPreviewClipFrameStatus cpuProvider;
  cpuProvider.clipId = QStringLiteral("source");
  cpuProvider.mediaOwnerClipId = cpuProvider.clipId;
  cpuProvider.presentedSourceFrame = 42;
  cpuProvider.frameSize = image.size();
  cpuProvider.frame = editor::FrameHandle::createCpuFrame(
      image, cpuProvider.presentedSourceFrame, QStringLiteral("/media/source.mp4"));

  VulkanPreviewClipFrameStatus cpuChild = cpuProvider;
  cpuChild.clipId = QStringLiteral("mask");
  QVERIFY(jcut::direct_vulkan_preview::mediaOwnerPayloadMatches(
      cpuProvider, cpuChild));

  cpuChild.presentedSourceFrame = 43;
  QVERIFY(!jcut::direct_vulkan_preview::mediaOwnerPayloadMatches(
      cpuProvider, cpuChild));
  cpuChild = cpuProvider;
  cpuChild.clipId = QStringLiteral("mask");
  cpuChild.frame = editor::FrameHandle::createCpuFrame(
      image, 43, QStringLiteral("/media/source.mp4"));
  QVERIFY(!jcut::direct_vulkan_preview::mediaOwnerPayloadMatches(
      cpuProvider, cpuChild));

  AVFrame* rawFrame = av_frame_alloc();
  QVERIFY(rawFrame != nullptr);
  rawFrame->format = AV_PIX_FMT_NV12;
  rawFrame->width = 8;
  rawFrame->height = 8;
  QCOMPARE(av_frame_get_buffer(rawFrame, 32), 0);
  VulkanPreviewClipFrameStatus hardwareProvider = cpuProvider;
  hardwareProvider.frame = editor::FrameHandle::createHardwareFrame(
      rawFrame, 42, QStringLiteral("/media/source.mp4"), AV_PIX_FMT_NV12);
  hardwareProvider.frameSize = hardwareProvider.frame.size();
  av_frame_free(&rawFrame);
  QVERIFY(hardwareProvider.frame.hasHardwareFrame());

  VulkanPreviewClipFrameStatus hardwareChild = hardwareProvider;
  hardwareChild.clipId = QStringLiteral("hardware-mask");
  QVERIFY(jcut::direct_vulkan_preview::mediaOwnerPayloadMatches(
      hardwareProvider, hardwareChild));
  hardwareChild.frame = cpuProvider.frame;
  QVERIFY(!jcut::direct_vulkan_preview::mediaOwnerPayloadMatches(
      hardwareProvider, hardwareChild));
}

void TestDirectVulkanHandoffPipelineContract::
    descriptorUpdatesFollowAcquiredSwapchainOwnership() {
  const QString resources = readSourceFile(QStringLiteral("vulkan_resources.cpp"));
  QVERIFY2(!resources.isEmpty(), "vulkan_resources.cpp must be readable");
  QVERIFY2(resources.contains(QStringLiteral(
               "m_descriptorSetIndex = frameSlot % m_descriptorSets.size()")),
           "descriptor selection must follow the acquired swapchain image");
  QVERIFY2(!resources.contains(QStringLiteral(
               "m_descriptorSetIndex = (m_descriptorSetIndex + 1) % m_descriptorSets.size()")),
           "sampled-image updates must not rotate onto a potentially pending descriptor set");
  QVERIFY2(resources.contains(QStringLiteral(
               "write.dstSet = m_descriptorSets[m_descriptorSetIndex]")),
           "mask recreation must update only the descriptor set owned by the current frame");
  QVERIFY2(resources.contains(QStringLiteral(
               "VUID-vkUpdateDescriptorSets-None-03047")),
           "the in-flight descriptor update regression must remain documented at the fix");

  const QString textRenderer = readSourceFile(QStringLiteral("vulkan_text_renderer.cpp"));
  QVERIFY2(textRenderer.contains(QStringLiteral(
               "entry.resources->setSampledImage(")) &&
               textRenderer.contains(QStringLiteral(
                   "entry.resources->sampledImageView()")),
           "an unchanged glyph atlas must still be rebound to the acquired frame's descriptor set");

  const QString preview = readSourceFile(QStringLiteral("direct_vulkan_preview_window.cpp"));
  const QString audio = readSourceFile(QStringLiteral("direct_vulkan_preview_audio.cpp"));
  QVERIFY2(preview.contains(QStringLiteral("const bool directAudioMode")) &&
               preview.contains(QStringLiteral("if (!directAudioMode) beginRenderPass()")) &&
               audio.contains(QStringLiteral("if (context.beginRenderPass) context.beginRenderPass()")),
           "audio uploads and compute dispatches must finish before the preview render pass begins");
}

void TestDirectVulkanHandoffPipelineContract::
    directPreviewRequiresHardwarePayloadsFromCache() {
  const QString source =
      readSourceFile(QStringLiteral("vulkan_preview_surface.cpp"));
  QVERIFY2(!source.isEmpty(), "vulkan_preview_surface.cpp must be readable");

  QVERIFY2(source.contains(QStringLiteral(
               "clip.mediaType != ClipMediaType::Image && visibleDecodeRequiresDirectVulkanPayload()")),
           "direct Vulkan preview must make the hardware/GPU visible-request "
           "requirement conditional on runtime CPU-upload fallback capability");
  QVERIFY2(source.contains(QStringLiteral("visibleCpuUploadFallbackEnabled()")) &&
               source.contains(QStringLiteral("visibleDecodeRequiresDirectVulkanPayload()")),
           "direct Vulkan preview must expose the runtime visible payload policy");
  QVERIFY2(
      source.contains(QStringLiteral("requireDirectVulkanPayload);")),
      "visible frame payload policy must be passed into frame requests");
  QVERIFY2(source.contains(QStringLiteral("directVulkanPreviewSupportsClip")) &&
               source.contains(QStringLiteral("clip.mediaType == ClipMediaType::Image")),
           "direct Vulkan preview must explicitly support still image clips");

  const QString backend =
      readSourceFile(QStringLiteral("direct_vulkan_preview_window.cpp"));
  QVERIFY2(!backend.isEmpty(), "direct_vulkan_preview_window.cpp must be readable");
  QVERIFY2(backend.contains(QStringLiteral("providerStatus.frame.hasCpuImage()")) &&
               backend.contains(QStringLiteral("providerStatus.frame.cpuImageBuffer()")) &&
               backend.contains(QStringLiteral("cb, *cpuBuffer")) &&
               backend.contains(QStringLiteral("cpu_image_upload")),
           "direct Vulkan preview must upload still-image CPU frames through "
           "the Vulkan texture path instead of the video handoff path");
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
  QVERIFY2(source.contains(QStringLiteral("!status.externalVulkanFrame && !status.frame.hasHardwareFrame()")),
           "handoff pipeline must reject CPU-only video frames instead of "
           "falling back to CPU image upload");
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
      textRenderer.contains(
          QStringLiteral("1,\n                                     &dynamicUniformOffset")) &&
          textRenderer.contains(
              QStringLiteral("atlasResources->frameUniformDynamicOffset()")),
      "Vulkan text descriptor binds must provide the dynamic uniform offset "
      "required by the shared descriptor layout");
  QVERIFY2(
      textRenderer.contains(QStringLiteral("prepareTranscriptOverlayAtlas")),
      "transcript glyph atlas upload must be available before the render pass");
  QVERIFY2(
      textRenderer.contains(QStringLiteral("prepareSpeakerLabelAtlas")),
      "speaker glyph atlas upload must be available before the render pass");
  QVERIFY2(textRenderer.contains(QStringLiteral("cachedResolvedFontFace")),
           "Vulkan text rendering must cache fontconfig face resolution "
           "outside the steady-state frame path");
  const QString textRendererHeader =
      readSourceFile(QStringLiteral("vulkan_text_renderer.h"));
  QVERIFY2(
      textRendererHeader.contains(QStringLiteral("QHash<QString, TitleLayoutCacheEntry> m_titleLayoutCache")) &&
          textRenderer.contains(QStringLiteral("stableTitle.x = 0.0")) &&
          textRenderer.contains(QStringLiteral("stableTitle.y = 0.0")) &&
          textRenderer.contains(QStringLiteral("stableTitle.opacity = 1.0")) &&
          textRenderer.contains(QStringLiteral("const QPointF animationOffset(title.x, title.y)")) &&
          textRenderer.contains(QStringLiteral("withTitleOpacity")),
      "animated title overlays must cache stable glyph/pattern/mesh layout "
      "across fly-in/fade frames and apply position/opacity at draw time");
  const QString exportBackend =
      readSourceFile(QStringLiteral("offscreen_vulkan_renderer_backend.cpp"));
  QVERIFY2(
      textRendererHeader.contains(QStringLiteral("transcriptOverlayAtlasNeedsUpload")) &&
          textRendererHeader.contains(QStringLiteral("titleOverlayAtlasNeedsUpload")) &&
          textRendererHeader.contains(QStringLiteral("std::vector<GpuAtlasCacheEntry> m_gpuAtlasCache")) &&
          textRendererHeader.contains(QStringLiteral("qsizetype bytes = 0")) &&
          textRendererHeader.contains(QStringLiteral("uploadFrameSlot")) &&
          textRendererHeader.contains(QStringLiteral("stagingReleaseEligible")) &&
          readSourceFile(QStringLiteral("vulkan_resources.h")).contains(QStringLiteral("releaseUploadStaging")) &&
          textRenderer.contains(QStringLiteral("kMaxResidentGpuTextAtlases")) &&
          textRenderer.contains(QStringLiteral("kMaxResidentGpuTextAtlasBytes")) &&
          textRenderer.contains(QStringLiteral("makeRoomForNewAtlas")) &&
          textRenderer.contains(QStringLiteral("entry.resources->releaseUploadStaging()")) &&
          textRenderer.contains(QStringLiteral("vkDeviceWaitIdle(m_device)")) &&
          textRenderer.contains(QStringLiteral("first_attempt=%1")) &&
          textRenderer.contains(QStringLiteral("m_atlasResources->descriptorSetLayout()")) &&
          textRenderer.contains(QStringLiteral("!atlasIsResident")) &&
          exportBackend.contains(QStringLiteral("const bool atlasUploadRequired")) &&
          exportBackend.contains(QStringLiteral("textRendererDrawRecorded && atlasUploadRequired")) &&
          !exportBackend.contains(QStringLiteral(
              "if (textRendererDrawRecorded) {\n"
              "          if (!finishTextDrawBeforeAtlasMutation())")),
      "offscreen export must not submit-and-wait between text overlays that "
      "reuse any resident atlas; only a missing atlas upload may force the "
      "text draw fence");
  const QString vulkanResources = readSourceFile(QStringLiteral("vulkan_resources.cpp"));
  QVERIFY2(vulkanResources.contains(QStringLiteral("texture_size_exceeds_device_limit")) &&
               vulkanResources.contains(QStringLiteral("texture_create_image_failed")) &&
               vulkanResources.contains(QStringLiteral("texture_allocate_memory_failed")) &&
               vulkanResources.contains(QStringLiteral("texture_bind_memory_failed")) &&
               vulkanResources.contains(QStringLiteral("texture_create_image_view_failed")),
           "large subtitle atlas uploads must report the exact Vulkan texture "
           "allocation failure instead of only the generic overlay texture size "
           "wrapper");
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
  const QString transcriptBackend =
      readSourceFile(QStringLiteral("direct_vulkan_preview_transcript.cpp"));
  QVERIFY2(!transcriptBackend.isEmpty(),
           "direct_vulkan_preview_transcript.cpp must be readable");
  QVERIFY2(transcriptBackend.contains(
               QStringLiteral("transcriptFrameForClipSourceFrame(effectiveClip,"
                              " status->presentedSourceFrame)")),
           "live Vulkan transcript subtitles must time against the presented "
           "video frame when one is available");
  const QString previewSurface =
      readSourceFile(QStringLiteral("vulkan_preview_surface.cpp"));
  QVERIFY2(!previewSurface.isEmpty(),
           "vulkan_preview_surface.cpp must be readable");
  QVERIFY2(previewSurface.contains(
               QStringLiteral("rawClipMaskBuffer(clip, status.frame)")) &&
               previewSurface.contains(
                   QStringLiteral("clip, markerStatus.frame")) &&
               previewSurface.contains(
                   QStringLiteral("clip,\n                            status.frameCrossfadeFrame")) &&
               !previewSurface.contains(QStringLiteral("maskFrameMatchesPresentedFrame")),
           "live Vulkan masks and mask grading must sample the mask for the "
           "exact parent presentation identity, including crossfade frames");
  const QString presenterSource =
      readSourceFile(QStringLiteral("direct_vulkan_preview_presenter.cpp"));
  QVERIFY2(!presenterSource.isEmpty(),
           "direct_vulkan_preview_presenter.cpp must be readable");
  QVERIFY2(presenterSource.contains(QStringLiteral("last_transcript_timing_source")) &&
               presenterSource.contains(QStringLiteral("last_transcript_timeline_sample")) &&
               presenterSource.contains(QStringLiteral("last_transcript_frame")) &&
               presenterSource.contains(QStringLiteral(
                   "last_transcript_presented_media_source_frame")),
           "direct Vulkan preview diagnostics must name transcript timing "
           "domains explicitly");
  QVERIFY2(!backend.contains(QStringLiteral("renderTranscriptOverlay(")),
           "direct Vulkan preview must not retain a CPU-rendered transcript "
           "overlay path");
}

void TestDirectVulkanHandoffPipelineContract::
    directPreviewDrawsAudioOnlyTranscriptOverlaysAfterVideoLoop() {
  const QString backend =
      readSourceFile(QStringLiteral("direct_vulkan_preview_window.cpp"));
  QVERIFY2(!backend.isEmpty(),
           "direct_vulkan_preview_window.cpp must be readable");

  const QString transcriptBackend =
      readSourceFile(QStringLiteral("direct_vulkan_preview_transcript.cpp"));
  QVERIFY2(!transcriptBackend.isEmpty(),
           "direct_vulkan_preview_transcript.cpp must be readable");

  QVERIFY2(transcriptBackend.contains(QStringLiteral("timelineTranscriptActive")),
           "direct preview must consider transcript overlays on active "
           "timeline clips even when no drawable video status exists");
  QVERIFY2(transcriptBackend.contains(QStringLiteral(
               "if (!statusDrawable && !timelineTranscriptActive)")),
           "transcript candidates must bypass the drawable-video status gate "
           "while their clip is active on the timeline");
  QVERIFY2(backend.contains(QStringLiteral(
               "QSet<QString> drawnTranscriptOverlayClipIds")),
           "direct preview must track which prepared transcript overlays were "
           "actually drawn");
  QVERIFY2(backend.contains(QStringLiteral(
               "for (auto it = preparedTranscriptOverlays.cbegin(); "
               "it != preparedTranscriptOverlays.cend(); ++it)")),
           "direct preview must run a fallback pass for prepared transcript "
           "overlays skipped by the video clip draw loop");
  QVERIFY2(backend.contains(QStringLiteral("fallbackTranscriptDrawCount")),
           "fallback transcript draws must be counted for diagnostics");
  QVERIFY2(backend.contains(QStringLiteral("textDrawStageMetric")),
           "draw-stage text telemetry must distinguish prepared overlays from "
           "visible overlays");
  QVERIFY2(backend.contains(QStringLiteral("transcriptCandidateCount")) &&
               backend.contains(QStringLiteral("transcriptPreparedCount")) &&
               backend.contains(QStringLiteral("transcriptDrawnCount")),
           "direct preview must expose simple transcript candidate/prepared/"
           "drawn counters for playback diagnostics");
  QVERIFY2(backend.contains(QStringLiteral("lastTranscriptSkipReason")),
           "direct preview must expose the last transcript skip reason");
  QVERIFY2(backend.contains(QStringLiteral("lastTextPrepFailureReason")) &&
               backend.contains(QStringLiteral("lastTextDrawFailureReason")),
           "direct preview must expose text prep/draw failure reasons");
  QVERIFY2(backend.contains(QStringLiteral(
               "m_lastPreparedTextReady =\n"
               "            !preparedTranscriptAtlasClipIds.isEmpty()")),
           "text prep cache hits must only be enabled after at least one "
           "transcript atlas was actually prepared, so a failed first prep "
           "does not permanently hide a paused audio-only transcript overlay");
  QVERIFY2(
      backend.indexOf(QStringLiteral("preparedTranscriptOverlays.insert(")) <
          backend.indexOf(QStringLiteral("drawnTranscriptOverlayClipIds")) &&
          backend.indexOf(QStringLiteral("drawnTranscriptOverlayClipIds")) <
              backend.indexOf(QStringLiteral("fallbackTranscriptDrawCount")),
      "audio-only transcript overlays must be prepared before draw tracking, "
      "and fallback draw telemetry must run after the main draw loop");

  const QString presenter =
      readSourceFile(QStringLiteral("direct_vulkan_preview_presenter.cpp"));
  QVERIFY2(!presenter.isEmpty(),
           "direct_vulkan_preview_presenter.cpp must be readable");
  QVERIFY2(presenter.contains(QStringLiteral("\"text_draw\"")) ||
               presenter.contains(QStringLiteral("text_draw")),
           "presenter diagnostics must expose the transcript text draw stage");
}

void TestDirectVulkanHandoffPipelineContract::
    exportUsesSharedTranscriptSourceResolution() {
  const QString exportBackend =
      readSourceFile(QStringLiteral("offscreen_vulkan_renderer_backend.cpp"));
  QVERIFY2(!exportBackend.isEmpty(),
           "offscreen_vulkan_renderer_backend.cpp must be readable");
  const qsizetype resolverIndex =
      exportBackend.indexOf(QStringLiteral("QString renderTranscriptPath"));
  QVERIFY2(resolverIndex >= 0,
           "offscreen Vulkan export must keep one local transcript resolver "
           "entry point for subtitle and speaker-label paths");
  const qsizetype resolverEnd =
      exportBackend.indexOf(QStringLiteral("VkRect2D scissorFromRect"),
                            resolverIndex);
  QVERIFY2(resolverEnd > resolverIndex,
           "offscreen Vulkan transcript resolver body must be bounded");
  const QString resolverBody =
      exportBackend.mid(resolverIndex, resolverEnd - resolverIndex);
  QVERIFY2(resolverBody.contains(
               QStringLiteral("activeTranscriptPathForClip(clip)")),
           "offscreen Vulkan export must resolve subtitle transcripts through "
           "the same clip-source helper as direct preview, including "
           "audio-backed transcript overlays");
  QVERIFY2(!resolverBody.contains(
               QStringLiteral("activeTranscriptPathForClipFile(clip.filePath)")),
           "offscreen Vulkan export must not fall back through clip.filePath; "
           "that loses external-audio transcript source identity");

  const QString previewBackend =
      readSourceFile(QStringLiteral("direct_vulkan_preview_transcript.cpp"));
  QVERIFY2(!previewBackend.isEmpty(),
           "direct_vulkan_preview_transcript.cpp must be readable");
  QVERIFY2(previewBackend.contains(
               QStringLiteral("activeTranscriptPathForClip(effectiveClip)")),
           "direct Vulkan preview must use the shared transcript source "
           "resolver that export is required to match");
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
               QStringLiteral("!discontinuityPrefetch && offset == 0")) &&
               playbackPipeline.contains(
                   QStringLiteral("? DecodeRequestKind::Visible")) &&
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
      playbackPipeline.contains(
          QStringLiteral("kind == DecodeRequestKind::Visible")) &&
          playbackPipeline.contains(
              QStringLiteral("discontinuityPrefetch ? qMax(20, 80 - offset)")) &&
          playbackPipeline.contains(QStringLiteral("qMax(10, 60 - offset)")),
      "prefetch priority must be materially lower than current visible "
      "priority");
  QVERIFY2(
      playbackPipeline.contains(
          QStringLiteral("void PlaybackFramePipeline::prefetchFramesForSample")) &&
          playbackPipeline.contains(
              QStringLiteral("m_pendingDiscontinuityPrefetchRequests")) &&
          playbackPipeline.contains(
              QStringLiteral("if (!discontinuityPrefetch) {\n"
                             "        const int64_t latencyRetentionFrames")) &&
          surface.contains(
              QStringLiteral("upcomingNoncontiguousPlaybackRangeStart")) &&
          surface.contains(
              QStringLiteral("m_playbackPipeline->prefetchFramesForSample")),
      "a noncontiguous range transition must prefetch through separate "
      "ownership before its crossfade window without advancing visible "
      "cancel-before ownership");

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
  QVERIFY2(playbackPipeline.contains(QStringLiteral("scheduleSingleFrame")) &&
               playbackPipeline.contains(QStringLiteral("info.isSingleFrame")) &&
               playbackPipeline.contains(QStringLiteral("m_decoder->requestFrame")) &&
               playbackPipeline.contains(QStringLiteral("it.value()->insert(0, frame)")),
           "PlaybackFramePipeline must schedule static images into its own "
           "buffer so still images and generated image effects remain drawable "
           "during active playback");
  QVERIFY2(surface.contains(QStringLiteral(
               "const int64_t localFrame = clip.mediaType == ClipMediaType::Image\n"
               "            ? 0\n"
               "            : sourceFrameForSample(clip, targetSample);")) &&
               surface.contains(QStringLiteral(
                   "const int64_t localFrame = clip.mediaType == ClipMediaType::Image\n"
                   "            ? 0\n"
                   "            : sourceFrameForSample(clip, visualSample);")),
           "direct-Vulkan playback readiness probes must normalize still images "
           "to frame 0 and test moving clips against the visual playback sample");
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
      vulkanSurface.contains(QStringLiteral("!m_interaction.playing")) &&
          vulkanSurface.contains(QStringLiteral("missingCount > 0")) &&
          vulkanSurface.contains(QStringLiteral("m_cache->pendingVisibleRequestCount() == 0")) &&
          vulkanSurface.contains(QStringLiteral("queueFrameStatusRefresh(true)")),
      "paused direct-Vulkan preview must retry visible decode when frame-status "
      "refresh discovers an active clip has no drawable frame");
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
  const QString decoderPolicyCore =
      readSourceFile(QStringLiteral("decoder_policy_core.cpp"));
  QVERIFY2(debugControls.contains(
               QStringLiteral("jcut::parseDecodePreferenceCore(")) &&
               decoderPolicyCore.contains(QStringLiteral(
                   "*preferenceOut = DecodePreferenceCore::HardwareZeroCopy;")),
           "the Qt adapter must delegate to the shared parser, where explicit "
           "hardware-zero-copy state survives for direct Vulkan playback");
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
  const qsizetype scopedExportSafetyIndex =
      renderExport.indexOf(QStringLiteral("struct ScopedRenderDecodeSafety"));
  const qsizetype scopedExportSafetyEnd =
      renderExport.indexOf(QStringLiteral("scopedDecodeSafety"), scopedExportSafetyIndex);
  QVERIFY2(scopedExportSafetyIndex >= 0 && scopedExportSafetyEnd > scopedExportSafetyIndex,
           "Vulkan export decode safety scope must be present and bounded");
  const QString scopedExportSafety =
      renderExport.mid(scopedExportSafetyIndex,
                       scopedExportSafetyEnd - scopedExportSafetyIndex);
  QVERIFY2(!scopedExportSafety.contains(QStringLiteral("setDebugDeterministicPipelineEnabled(true)")),
           "Vulkan export must not force the global deterministic debug "
           "pipeline; deterministic output ordering should not serialize the "
           "GPU throughput path by default");
  const QString editorDeterministicSource = readSourceFile(QStringLiteral("editor.cpp"));
  QVERIFY2(!editorDeterministicSource.isEmpty(), "editor.cpp must be readable");
  const QString projectDeterministicState = readSourceFile(QStringLiteral("project_state.cpp"));
  QVERIFY2(!projectDeterministicState.isEmpty(), "project_state.cpp must be readable");
  QVERIFY2(editorDeterministicSource.contains(QStringLiteral("debugDeterministicPipelineExplicit")) &&
               editorDeterministicSource.contains(QStringLiteral(": false")) &&
               projectDeterministicState.contains(QStringLiteral("debugDeterministicPipelineExplicit")),
           "preview/export throughput must migrate old accidental deterministic "
           "state back to the fast path unless the project state has an "
           "explicit deterministic-pipeline marker");
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

  const QString directPreviewPresenter =
      readSourceFile(QStringLiteral("direct_vulkan_preview_presenter.cpp"));
  QVERIFY2(!directPreviewPresenter.isEmpty(),
           "direct_vulkan_preview_presenter.cpp must be readable");
  const QStringList directPreviewFields{
      QStringLiteral("presented_frames"),
      QStringLiteral("preview_update_requests"),
      QStringLiteral("preview_update_events_delivered"),
      QStringLiteral("preview_updates_delivered"),
      QStringLiteral("preview_updates_deferred_not_exposed"),
      QStringLiteral("preview_updates_discarded_not_exposed"),
      QStringLiteral("stale_preview_update_recoveries"),
      QStringLiteral("last_stale_preview_update_age_ms"),
      QStringLiteral("last_preview_update_latency_ms"),
      QStringLiteral("max_preview_update_latency_ms"),
      QStringLiteral("unique_presentation_misses")};
  for (const QString &field : directPreviewFields) {
    QVERIFY2(directPreviewPresenter.contains(field),
             qPrintable(QStringLiteral(
                            "direct preview diagnostics must expose %1")
                            .arg(field)));
  }

  const QString routes =
      readSourceFile(QStringLiteral("control_server_worker_routes.cpp"));
  QVERIFY2(!routes.isEmpty(),
           "control_server_worker_routes.cpp must be readable");
  const QStringList liveCadenceFields{
      QStringLiteral("presented_frames"),
      QStringLiteral("preview_update_requests"),
      QStringLiteral("preview_update_events_delivered"),
      QStringLiteral("preview_updates_delivered")};
  for (const QString &field : liveCadenceFields) {
    QVERIFY2(
        routes.contains(field),
        qPrintable(QStringLiteral(
                       "live playback diagnostics must expose %1 for "
                       "counter-over-wall-time cadence probes")
                       .arg(field)));
  }
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
  const qsizetype playbackStagesStart = editorProfiling.indexOf(
      QStringLiteral("QJsonObject EditorWindow::playbackStageMetricsSnapshot("));
  const qsizetype playbackStagesEnd = editorProfiling.indexOf(
      QStringLiteral("void appendRuntimePatch("), playbackStagesStart);
  const QString playbackStagesBody =
      playbackStagesStart >= 0 && playbackStagesEnd > playbackStagesStart
          ? editorProfiling.mid(
                playbackStagesStart,
                playbackStagesEnd - playbackStagesStart)
          : QString();
  QVERIFY2(
      playbackStagesBody.contains(QStringLiteral("previewSnapshot.value(")) &&
          !playbackStagesBody.contains(
              QStringLiteral("m_preview->profilingSnapshot()")),
      "stage metrics must merge the already-collected compact/full preview "
      "snapshot instead of triggering a second full UI profile");

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
  QVERIFY2(vulkanSurface.contains(QStringLiteral("m_presenter->requestPipelineTapReadback()")) &&
               vulkanSurface.contains(QStringLiteral("m_presenter->latestPipelineTapImage()")),
           "pipeline stages must request and display the composed GPU tap image");
  QVERIFY2(vulkanSurface.contains(QStringLiteral("13 Diagnostic Readback")) &&
               vulkanSurface.contains(QStringLiteral("diagnostic_disabled")),
           "pipeline stages must report diagnostic readback as opt-in, not as "
           "a hot-path render dependency");

  const QString presenter =
      readSourceFile(QStringLiteral("direct_vulkan_preview_presenter.cpp"));
  QVERIFY2(presenter.contains(QStringLiteral("directVulkanPreviewWindowPipelineThumbnailReadbackPending")),
           "pipeline tap pending state must be reported from the live Vulkan window");
}

void TestDirectVulkanHandoffPipelineContract::
    playbackTelemetryUsesCanonicalAtomicsWithoutUiInvocation() {
  const QString routes =
      readSourceFile(QStringLiteral("control_server_worker_routes.cpp"));
  QVERIFY2(!routes.isEmpty(),
           "control_server_worker_routes.cpp must be readable");
  const qsizetype telemetryStart =
      routes.indexOf(QStringLiteral("/playback/telemetry"));
  const qsizetype syncStart =
      routes.indexOf(QStringLiteral("/playback/sync"), telemetryStart);
  const QString telemetryRoute =
      telemetryStart >= 0 && syncStart > telemetryStart
          ? routes.mid(telemetryStart, syncStart - telemetryStart)
          : QString();
  QVERIFY2(!telemetryRoute.isEmpty(),
           "playback telemetry must have a dedicated route");
  QVERIFY2(telemetryRoute.contains(QStringLiteral("fastSnapshot()")),
           "playback telemetry must read the worker-safe fast snapshot");
  QVERIFY2(
      !telemetryRoute.contains(QStringLiteral("invokeOnUiThread")) &&
          !telemetryRoute.contains(
              QStringLiteral("refreshPipelineSnapshotFromUi")) &&
          !telemetryRoute.contains(QStringLiteral("m_pipelineSnapshotCallback")),
      "playback telemetry polling must never enqueue or wait for UI work");

  const QString presenterHeader =
      readSourceFile(QStringLiteral("direct_vulkan_preview_presenter.h"));
  const QString presenterWindow =
      readSourceFile(QStringLiteral("direct_vulkan_preview_window.cpp"));
  QVERIFY2(!presenterHeader.isEmpty() && !presenterWindow.isEmpty(),
           "direct Vulkan telemetry sources must be readable");
  const QStringList requiredAtomicFields{
      QStringLiteral("presentedFrames"),
      QStringLiteral("uniquePresentationMisses"),
      QStringLiteral("previewUpdateRequests"),
      QStringLiteral("previewUpdateEventsDelivered"),
      QStringLiteral("previewUpdatesDelivered"),
      QStringLiteral("activeRequestedSourceFrame"),
      QStringLiteral("activePresentedSourceFrame")};
  for (const QString &field : requiredAtomicFields) {
    QVERIFY2(
        presenterHeader.contains(
            QStringLiteral("std::atomic<int64_t> %1").arg(field)),
        qPrintable(QStringLiteral("%1 must have canonical atomic ownership")
                       .arg(field)));
  }
  QVERIFY2(
      presenterWindow.contains(QStringLiteral(
          "m_presentationTelemetry->presentedFrames.fetch_add")) &&
          presenterWindow.contains(QStringLiteral(
              "m_presentationTelemetry->uniquePresentationMisses.fetch_add")) &&
          presenterWindow.contains(QStringLiteral(
              "m_presentationTelemetry->activeRequestedSourceFrame.store")) &&
          presenterWindow.contains(QStringLiteral(
              "m_presentationTelemetry->activePresentedSourceFrame.store")),
      "presentation callbacks must publish directly to canonical atomics");

  const QString editorHeader = readSourceFile(QStringLiteral("editor.h"));
  QVERIFY2(!editorHeader.contains(QStringLiteral("m_fastPresentedFrames")) &&
               !editorHeader.contains(
                   QStringLiteral("m_fastUniquePresentationMisses")),
           "EditorWindow must not retain sampled shadow ownership of presenter "
           "counters");

  const QString setup = readSourceFile(QStringLiteral("editor_setup.cpp"));
  const QStringList requiredRouteFields{
      QStringLiteral("transport_timeline_sample"),
      QStringLiteral("projected_audio_feedback_timeline_sample"),
      QStringLiteral("projected_audio_feedback_timeline_frame"),
      QStringLiteral("audio_clock_available"),
      QStringLiteral("has_playable_audio"),
      QStringLiteral("audio_playback_blocked"),
      QStringLiteral("pitch_preserving_audio_blocked"),
      QStringLiteral("time_stretch_cache_miss_count"),
      QStringLiteral("audio_underrun_count"),
      QStringLiteral("presented_frames"),
      QStringLiteral("unique_presentation_misses"),
      QStringLiteral("preview_update_requests"),
      QStringLiteral("preview_update_events_delivered"),
      QStringLiteral("preview_updates_delivered"),
      QStringLiteral("active_requested_source_frame"),
      QStringLiteral("active_presented_source_frame")};
  for (const QString &field : requiredRouteFields) {
    QVERIFY2(
        setup.contains(field),
        qPrintable(QStringLiteral(
                       "fast playback telemetry must expose %1 at top level")
                       .arg(field)));
  }
  QVERIFY2(
      setup.contains(QStringLiteral(
          "m_fastPlaybackSyncTelemetryRevision.load(")) &&
          setup.contains(QStringLiteral("revisionBefore == revisionAfter")),
      "sample-domain A/V telemetry must be read from one revision-bracketed "
      "playback tick");

  const QString playback =
      readSourceFile(QStringLiteral("editor_playback.cpp"));
  QVERIFY2(
      playback.contains(
          QStringLiteral("void EditorWindow::publishFastPlaybackSyncTelemetry()")) &&
          playback.contains(QStringLiteral(
              "m_fastPlaybackSyncTelemetryRevision.fetch_add(")) &&
          playback.contains(
              QStringLiteral("timelineSampleForAudioFeedbackSample(")),
      "the UI clock tick must publish absolute transport and projected audio "
      "feedback samples without changing clock ownership");
}

void TestDirectVulkanHandoffPipelineContract::
    transportControlDoesNotCollectUiProfiles() {
  const QString routes =
      readSourceFile(QStringLiteral("control_server_worker_routes_ui.cpp"));
  QVERIFY2(!routes.isEmpty(),
           "control_server_worker_routes_ui.cpp must be readable");
  QVERIFY2(
      routes.contains(QStringLiteral("const bool transportControlRequest =")) &&
          routes.contains(QStringLiteral("if (transportControlRequest)")),
      "play and pause must use the dedicated lightweight control path in every "
      "windowing mode");

  const qsizetype transportStart =
      routes.indexOf(QStringLiteral("if (transportControlRequest)"));
  const qsizetype genericClickStart = routes.indexOf(
      QStringLiteral("const int requestTimeoutMs ="), transportStart);
  const QString transportPath =
      transportStart >= 0 && genericClickStart > transportStart
          ? routes.mid(transportStart, genericClickStart - transportStart)
          : QString();
  QVERIFY2(!transportPath.isEmpty(),
           "dedicated transport control path must precede generic click handling");
  QVERIFY2(
      !transportPath.contains(QStringLiteral("m_profilingCallback")),
      "transport control must not synchronously collect a full UI profile while "
      "playback is starting or stopping");
  QVERIFY2(
      transportPath.contains(QStringLiteral(
          "id == QStringLiteral(\"transport.pause\")")) &&
          transportPath.contains(
              QStringLiteral("\"playback_timer_active\"")) &&
          transportPath.contains(
              QStringLiteral("\"state_changed\"), false")),
      "transport.pause must be idempotent instead of toggling an already-paused "
      "transport back into playback");
}

void TestDirectVulkanHandoffPipelineContract::
    latestPresentedFrameImageExposesCpuPresentedFrame() {
  const QString surface = readSourceFile(QStringLiteral("vulkan_preview_surface.cpp"));
  QVERIFY2(!surface.isEmpty(), "vulkan_preview_surface.cpp must be readable");

  const QString signature =
      QStringLiteral("QImage VulkanPreviewSurface::latestPresentedFrameImageForClip");
  const qsizetype start = surface.indexOf(signature);
  QVERIFY2(start >= 0,
           "Vulkan preview must implement latestPresentedFrameImageForClip");
  const qsizetype end =
      surface.indexOf(QStringLiteral("QVector<PreviewSurface::PipelineStageSnapshot> "
                                     "VulkanPreviewSurface::livePipelineSnapshots"),
                      start);
  QVERIFY2(end > start,
           "latestPresentedFrameImageForClip must be isolated before live pipeline snapshots");

  const QString body = surface.mid(start, end - start);
  QVERIFY2(body.contains(QStringLiteral(
               "clipSelectionContext(&*clipIt, m_interaction.clips).owner()")) &&
               body.contains(QStringLiteral(
                   "m_lastPresentedFrameByClip.value(mediaOwnerClipId)")),
           "grading must read the selected clip's media owner so generated children "
           "reuse their parent's actual presented frame");
  QVERIFY2(body.contains(QStringLiteral("frame.hasCpuImage()")),
           "latest presented image must keep the direct path for CPU-backed still frames");
  QVERIFY2(body.contains(QStringLiteral("return frame.cpuImage()")),
           "latest presented image must return the presented CPU frame image");
  QVERIFY2(body.contains(QStringLiteral("render_detail::frameHandleToCpuImage(frame)")),
           "paused grading histogram must materialize the selected presented hardware "
           "video frame instead of returning an empty image");
  QVERIFY2(!body.contains(QStringLiteral("Q_UNUSED(clipId)")),
           "latest presented image must not be an empty stub");
}

void TestDirectVulkanHandoffPipelineContract::
    playbackReadinessRequiresExactFrames() {
  const QString surface = readSourceFile(QStringLiteral("vulkan_preview_surface.cpp"));
  QVERIFY2(!surface.isEmpty(), "vulkan_preview_surface.cpp must be readable");

  const QString prepareSignature =
      QStringLiteral("bool VulkanPreviewSurface::preparePlaybackAdvanceSample");
  const qsizetype prepareStart = surface.indexOf(prepareSignature);
  const qsizetype lookaheadStart =
      surface.indexOf(QStringLiteral("bool VulkanPreviewSurface::hasPlaybackLookaheadBuffered"),
                      prepareStart);
  QVERIFY2(prepareStart >= 0 && lookaheadStart > prepareStart,
           "preparePlaybackAdvanceSample must precede lookahead readiness");
  const QString prepareBody = surface.mid(prepareStart, lookaheadStart - prepareStart);
  QVERIFY2(prepareBody.contains(QStringLiteral("m_playbackPipeline->getFrame(clip.id, localFrame)")),
           "playback advance readiness must require the exact target frame, "
           "not a stale presentation fallback");
  QVERIFY2(!prepareBody.contains(QStringLiteral("getPresentationFrame(clip.id, localFrame)")),
           "playback advance readiness must not accept approximate presentation frames");

  const qsizetype currentStart =
      surface.indexOf(QStringLiteral("bool VulkanPreviewSurface::currentPlaybackFrameReadyForStart"),
                      lookaheadStart);
  QVERIFY2(currentStart > lookaheadStart,
           "current playback readiness must be present after lookahead readiness");
  const QString lookaheadBody = surface.mid(lookaheadStart, currentStart - lookaheadStart);
  QVERIFY2(lookaheadBody.contains(QStringLiteral("m_playbackPipeline->getFrame(clip.id, localFrame)")),
           "playback lookahead readiness must require exact buffered frames");
}

void TestDirectVulkanHandoffPipelineContract::
    playbackPipelineUsesTransportSampleDomain() {
  const QString pipeline =
      readSourceFile(QStringLiteral("playback_frame_pipeline.cpp"));
  const QString header =
      readSourceFile(QStringLiteral("playback_frame_pipeline.h"));
  const QString surface =
      readSourceFile(QStringLiteral("vulkan_preview_surface.cpp"));
  const QString playback =
      readSourceFile(QStringLiteral("editor_playback.cpp"));
  QVERIFY2(!pipeline.isEmpty() && !header.isEmpty() && !surface.isEmpty() &&
               !playback.isEmpty(),
           "transport timing sources must be readable");

  QVERIFY2(header.contains(QStringLiteral("void setPlayheadSample(int64_t playheadSample)")) &&
               header.contains(QStringLiteral("std::atomic<int64_t> m_playheadSample")),
           "playback pipeline ownership must track the monotonic transport "
           "sample, not only the rounded timeline frame");

  const qsizetype setStart = pipeline.indexOf(
      QStringLiteral("void PlaybackFramePipeline::setPlayheadSample"));
  const qsizetype speedStart = pipeline.indexOf(
      QStringLiteral("void PlaybackFramePipeline::setPlaybackSpeed"), setStart);
  const QString setBody =
      setStart >= 0 && speedStart > setStart
          ? pipeline.mid(setStart, speedStart - setStart)
          : QString();
  QVERIFY2(setBody.contains(QStringLiteral("samplesToFramePosition(playheadSample)")) &&
               setBody.contains(QStringLiteral(
                   "dropStaleRequestsForPlayheadSample(playheadSample)")),
           "pipeline playhead diagnostics may expose frames, but pruning must "
           "be anchored by the transport sample");

  const qsizetype dropStart = pipeline.indexOf(
      QStringLiteral("void PlaybackFramePipeline::dropStaleRequestsForPlayheadSample"));
  const qsizetype requestStart = pipeline.indexOf(
      QStringLiteral("void PlaybackFramePipeline::requestFramesForSample"), dropStart);
  const QString dropBody =
      dropStart >= 0 && requestStart > dropStart
          ? pipeline.mid(dropStart, requestStart - dropStart)
          : QString();
  QVERIFY2(dropBody.contains(QStringLiteral("clipTimelineStartSamples(info.clip)")) &&
               dropBody.contains(QStringLiteral("clipTimelineEndSamples(info.clip)")) &&
               dropBody.contains(QStringLiteral(
                   "sourceFrameForClipAtTimelineSample(info.clip, playheadSample, markers)")) &&
               !dropBody.contains(QStringLiteral("sourceFrameForClipAtTimelinePosition")),
           "stale-request pruning must use the shared sample-domain "
           "timeline-to-source conversion");

  const qsizetype readyStart =
      pipeline.indexOf(QStringLiteral("void PlaybackFramePipeline::onFrameReady"));
  const qsizetype keyStart =
      pipeline.indexOf(QStringLiteral("QString PlaybackFramePipeline::requestKey"), readyStart);
  const QString readyBody =
      readyStart >= 0 && keyStart > readyStart
          ? pipeline.mid(readyStart, keyStart - readyStart)
          : QString();
  QVERIFY2(readyBody.contains(QStringLiteral(
                   "const int64_t playheadSample = m_playheadSample.load()")) &&
               readyBody.contains(QStringLiteral(
                   "sourceFrameForClipAtTimelineSample(info.clip, playheadSample, markers)")),
           "decoder completion seeding must classify late frames against the "
           "same sample-domain transport playhead");

  const qsizetype prepareStart = surface.indexOf(QStringLiteral(
      "bool VulkanPreviewSurface::preparePlaybackAdvanceSample"));
  const qsizetype lookaheadStart = surface.indexOf(QStringLiteral(
      "bool VulkanPreviewSurface::hasPlaybackLookaheadBuffered"), prepareStart);
  const QString prepareBody =
      prepareStart >= 0 && lookaheadStart > prepareStart
          ? surface.mid(prepareStart, lookaheadStart - prepareStart)
          : QString();
  QVERIFY2(prepareBody.contains(QStringLiteral(
                   "m_playbackPipeline->setPlayheadSample(targetSample)")) &&
               !prepareBody.contains(QStringLiteral("requestFramesForSample")),
           "preview advance preparation must move decode ownership to the "
           "target transport sample before the UI frame is applied, while "
           "leaving scheduling centralized in requestFramesForCurrentPosition");

  QVERIFY2(playback.contains(QStringLiteral(
                   "m_preview->preparePlaybackAdvanceSample(nextSample)")) &&
               !playback.contains(QStringLiteral(
                   "m_preview->preparePlaybackAdvance(nextFrame)")),
           "EditorWindow::advanceFrame must pass the exact monotonic "
           "transport sample to video follower preparation");
}

void TestDirectVulkanHandoffPipelineContract::
    gradingPreviewControlRestoresSelectedTrackState() {
  const QString editorSource = readSourceFile(QStringLiteral("editor.cpp"));
  const QString tabsSource = readSourceFile(QStringLiteral("editor_tabs.cpp"));
  const QString bindingsSource =
      readSourceFile(QStringLiteral("editor_inspector_bindings.cpp"));
  QVERIFY2(!editorSource.isEmpty() && !tabsSource.isEmpty() &&
               !bindingsSource.isEmpty(),
           "grading preview UI sources must be readable");

  QVERIFY2(!editorSource.contains(QStringLiteral(
               "m_bypassGradingCheckBox->setChecked(true)")) &&
               editorSource.contains(QStringLiteral(
                   "selectedTrack ? selectedTrack->gradingPreviewEnabled : false")),
           "project load must restore the Preview checkbox from the selected "
           "track state instead of forcing grading preview on");
  QVERIFY2(tabsSource.contains(QStringLiteral(
                   "track ? track->gradingPreviewEnabled : false")) &&
               bindingsSource.contains(QStringLiteral(
                   "track.gradingPreviewEnabled = checked")) &&
               bindingsSource.contains(QStringLiteral(
                   "m_preview->setBypassGrading(false)")),
           "the Preview checkbox must remain a per-track grading visibility "
           "control, not a global bypass toggle");
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
    streamTimingDiagnosticsUseEffectiveProxyState() {
  const QString editorProfiling =
      readSourceFile(QStringLiteral("editor_profiling.cpp"));
  QVERIFY2(!editorProfiling.isEmpty(),
           "editor_profiling.cpp must be readable");
  QVERIFY2(editorProfiling.contains(QStringLiteral("effectivePreviewClip")) &&
               editorProfiling.contains(
                   QStringLiteral("effectivePreviewClip.useProxy = false")) &&
               editorProfiling.contains(
                   QStringLiteral("effectivePreviewClip.proxyPath.clear()")),
           "stream timing diagnostics must resolve playback_media_path from "
           "the same effective proxy state as preview playback");
  QVERIFY2(editorProfiling.contains(
               QStringLiteral("\"configured_playback_media_path\"")) &&
               editorProfiling.contains(
                   QStringLiteral("\"configured_proxy_media_path\"")) &&
               editorProfiling.contains(
                   QStringLiteral("\"effective_proxy_enabled\"")),
           "stream timing diagnostics must expose configured proxy state "
           "separately from the effective preview playback path");

  const QString editorTabs = readSourceFile(QStringLiteral("editor_tabs.cpp"));
  QVERIFY2(!editorTabs.isEmpty(), "editor_tabs.cpp must be readable");
  QVERIFY2(editorTabs.contains(QStringLiteral("effectivePreviewClip")) &&
               editorTabs.contains(
                   QStringLiteral("m_renderUseProxiesCheckBox")) &&
               editorTabs.contains(
                   QStringLiteral("effectivePreviewClip.useProxy = false")),
           "the properties tab must report proxy usage from the effective "
           "preview path, not only the clip's configured proxy flag");
}

void TestDirectVulkanHandoffPipelineContract::
    timelineUseProxyMenuControlsEffectiveProxyState() {
  const QString header = readSourceFile(QStringLiteral("timeline_widget.h"));
  QVERIFY2(!header.isEmpty(), "timeline_widget.h must be readable");
  QVERIFY2(header.contains(QStringLiteral("proxyPlaybackEnabled")) &&
               header.contains(QStringLiteral("proxyPlaybackEnabledChanged")),
           "timeline proxy menu must be wired to the global effective proxy "
           "playback switch");

  const QString menu =
      readSourceFile(QStringLiteral("timeline_widget_context_menu.cpp"));
  QVERIFY2(!menu.isEmpty(),
           "timeline_widget_context_menu.cpp must be readable");
  QVERIFY2(menu.contains(QStringLiteral("proxyPlaybackIsEnabled")) &&
               menu.contains(QStringLiteral(
                   "m_clips[clipIndex].useProxy && proxyPlaybackIsEnabled")),
           "the Use Proxy menu checkmark must reflect effective proxy use, "
           "not only the per-clip eligibility flag");
  QVERIFY2(menu.contains(QStringLiteral("if (useProxyAction->isChecked()")) &&
               menu.contains(QStringLiteral("proxyPlaybackEnabledChanged(true)")),
           "selecting Use Proxy on must enable the global proxy playback "
           "switch without turning it off for other clips when one clip is "
           "disabled");

  const QString editorPane =
      readSourceFile(QStringLiteral("editor_editor_pane.cpp"));
  QVERIFY2(!editorPane.isEmpty(), "editor_editor_pane.cpp must be readable");
  QVERIFY2(editorPane.contains(QStringLiteral("m_timeline->proxyPlaybackEnabled")) &&
               editorPane.contains(
                   QStringLiteral("m_renderUseProxiesCheckBox->setChecked(enabled)")),
           "editor wiring must map the timeline proxy menu to the shared "
           "preview/export proxy checkbox");
}

void TestDirectVulkanHandoffPipelineContract::
    timelineContextMenuControlsClipRenderVisibility() {
  const QString menu =
      readSourceFile(QStringLiteral("timeline_widget_context_menu.cpp"));
  QVERIFY2(!menu.isEmpty(),
           "timeline_widget_context_menu.cpp must be readable");
  QVERIFY2(menu.contains(QStringLiteral("Render Visible")) &&
               menu.contains(QStringLiteral("renderVisibleAction->setCheckable(true)")) &&
               menu.contains(QStringLiteral("renderVisibleAction->setChecked(m_clips[clipIndex].videoEnabled)")) &&
               menu.contains(QStringLiteral("clip.videoEnabled = renderVisibleAction->isChecked()")),
           "the clip context menu must expose render visibility as the "
           "existing persisted videoEnabled flag used by preview and export");
  QVERIFY2(menu.contains(QStringLiteral("clipHasVisuals(m_clips[clipIndex])")) &&
               menu.contains(QStringLiteral("!m_clips[clipIndex].locked")) &&
               menu.contains(QStringLiteral("if (clipsChanged) clipsChanged();")) &&
               menu.contains(QStringLiteral("update();")),
           "render visibility changes must be limited to editable visual clips "
           "and immediately publish through the normal timeline change path");
  const QString media =
      readSourceFile(QStringLiteral("editor_shared_media.cpp"));
  const QString gpu =
      readSourceFile(QStringLiteral("render_gpu.cpp"));
  QVERIFY2(media.contains(QStringLiteral("return clipHasVisuals(clip) && clip.videoEnabled")) &&
               gpu.contains(QStringLiteral("clipContributesVisualMedia(clip, clips, tracks")),
           "preview/export must consume the same videoEnabled visibility "
           "contract instead of a separate UI-only hidden flag");
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
           "playback must inspect exact retimed-audio readiness at the "
           "current frame");
  QVERIFY2(
      playback.contains(QStringLiteral("requestPlaybackAudioWarmup()")) &&
          playback.contains(QStringLiteral(
              "audio follower waiting for re-timed audio")),
      "missing retimed audio at startup must enter warmup/generation as an "
      "audio follower without delaying transport start");
  QVERIFY2(
      !playback.contains(QStringLiteral(
          "transport playback waiting while pitch-preserving audio warms")),
      "active playback must not hold the system transport when required "
      "retimed audio is not ready");
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
  QVERIFY2(source.contains(QStringLiteral("timingSource.sourceFrameSize.isValid()")) &&
               source.contains(QStringLiteral("? timingSource.sourceFrameSize")),
           "export video placement must prefer the resolved timing owner's "
           "sourceFrameSize before decoded payload size so a Mask Matte and "
           "its parent share transform geometry");
  QVERIFY2(source.contains(QStringLiteral("renderer_texture_origin")) &&
               source.contains(QStringLiteral("renderer_texture_normalized")),
           "export diagnostics must state the renderer texture contract instead "
           "of deriving placement from texture-origin branches");
  QVERIFY2(!source.contains(QStringLiteral("TextureOrigin textureOrigin")) &&
               !source.contains(QStringLiteral("textureOriginRequiresExportYFlip(layer.textureOrigin")) &&
               !source.contains(QStringLiteral("exportVideoLayerTranslationForSampledFace")),
           "export video placement must consume canonical top-left renderer "
           "textures without texture-origin-specific transform compensation");
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
               "evaluateClipRenderTransformWithSourceLockAtPosition(\n"
               "            clip,\n"
               "            request.clips,\n"
               "            transformClockTimelineFrame,\n"
               "            request.renderSyncMarkers,\n"
               "            request.playbackTiming,\n"
               "            request.outputSize,\n"
               "            &transformDiagnostics)")),
           "export must pass request.renderSyncMarkers and request.playbackTiming "
           "into render transform evaluation so speaker framing targets the same "
           "face box and transcript play time as preview");
  QVERIFY2(exportRenderer.contains(QStringLiteral(
               "exportFaceTransformDiagnostics")),
           "export must expose face-box/transform diagnostics from the same "
           "speaker-framing transform path");

  const QString preview =
      readSourceFile(QStringLiteral("vulkan_preview_surface.cpp"));
  QVERIFY2(!preview.isEmpty(), "vulkan_preview_surface.cpp must be readable");
  QVERIFY2(preview.contains(QStringLiteral(
               "evaluateClipRenderTransformWithSourceLockAtPosition(\n"
               "            clip,\n"
               "            m_interaction.clips,\n"
               "            transformFramePosition,\n"
               "            m_interaction.renderSyncMarkers,\n"
               "            m_interaction.playbackTiming,\n"
               "            m_interaction.outputSize)")),
           "preview must pass its interaction render sync markers and playback "
           "timing into the same render transform evaluation path");
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
  const QString sectionCore =
      readSourceFile(QStringLiteral("speaker_section_core.cpp"));
  QVERIFY2(!tracks.isEmpty(), "tracks.cpp must be readable");
  QVERIFY2(!sectionCore.isEmpty(), "speaker_section_core.cpp must be readable");
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
  QVERIFY2(
      tracks.contains(QStringLiteral("setSpeakerSectionTrackAssignmentsCore")) &&
          tracks.contains(QStringLiteral("anchors,\n                false")) &&
          sectionCore.contains(QStringLiteral("if (!replaceExisting)")) &&
          sectionCore.contains(QStringLiteral("nextTracks = currentTracks")) &&
          sectionCore.contains(
              QStringLiteral("if (!replaced) nextTracks.push_back")),
           "assigning another track to matching contiguous transcript sections "
           "must use the shared mutation core in merge mode so existing rows "
           "and tracks are preserved and extended");
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
  QVERIFY2(speakers.contains(QStringLiteral("manualPreviewAssignedFaceTrackIdsForClip")) &&
               speakers.contains(QStringLiteral("clip.speakerFramingManualTrackId >= 0")) &&
               speakers.contains(QStringLiteral("return manualPreviewAssignedFaceTrackIdsForClip(clip)")),
           "preview track selection sync must fall back to the persisted manual "
           "FaceDetections track when no transcript speaker is active after restart");
  QVERIFY2(speakers.contains(QStringLiteral("m_speakerDeps.isPlaybackActive")) &&
               speakers.contains(QStringLiteral("syncCurrentSpeakerSentenceToPlayhead(true)")) &&
               speakers.contains(QStringLiteral("if (playbackActive)")),
           "playback playhead sync must not auto-select speakers/sections or "
           "replace the user's current face-track allocation context");
  const qsizetype syncIdentityIndex =
      speakers.indexOf(QStringLiteral("void SpeakersTab::syncIdentityToPlayhead"));
  const qsizetype selectSpeakerIndex =
      speakers.indexOf(QStringLiteral("bool SpeakersTab::selectSpeakerRowById"), syncIdentityIndex);
  QVERIFY2(syncIdentityIndex >= 0 && selectSpeakerIndex > syncIdentityIndex,
           "speaker identity sync body must be bounded for playback-context checks");
  const QString syncIdentityBody =
      speakers.mid(syncIdentityIndex, selectSpeakerIndex - syncIdentityIndex);
  const qsizetype playbackGuardIndex =
      syncIdentityBody.indexOf(QStringLiteral("if (playbackActive)"));
  const qsizetype activeSpeakerLookupIndex =
      syncIdentityBody.indexOf(QStringLiteral("activeSpeakerIdInTranscriptRootAtSourceFrame"));
  QVERIFY2(playbackGuardIndex >= 0 && activeSpeakerLookupIndex > playbackGuardIndex,
           "playback context guard must run before active-speaker selection sync");
  const QString playbackGuardBody =
      syncIdentityBody.mid(playbackGuardIndex, activeSpeakerLookupIndex - playbackGuardIndex);
  QVERIFY2(!playbackGuardBody.contains(QStringLiteral("m_lastPlayheadSynced")),
           "playback context guard must not update full-sync markers; stopping "
           "playback must force a normal speaker/track sync at the final frame");
  const QString interactions =
      readSourceFile(QStringLiteral("speakers_tab_interactions.cpp"));
  QVERIFY2(!interactions.isEmpty(), "speakers_tab_interactions.cpp must be readable");
  QVERIFY2(interactions.contains(QStringLiteral("previewAssignedFaceTrackIdsForSpeakerAtFrame")) &&
               interactions.contains(QStringLiteral("currentSourceFrameForClip(*clip)")) &&
               interactions.contains(QStringLiteral("playheadAssignedTrackIdsForSpeaker")),
           "selected-speaker panel refresh must not clear contiguous "
           "section-track preview assignments when continuity streams are "
           "stored in indexed artifacts");
  QVERIFY2(!interactions.contains(QStringLiteral("setPreviewAssignedFaceTrackIds({})")) &&
               interactions.contains(QStringLiteral("manualPreviewAssignedFaceTrackIdsForClip(*clip)")),
           "selected-speaker panel refresh must not erase a saved manual "
           "facebox track when there is no selected speaker");
  QVERIFY2(
      speakers.contains(QStringLiteral("speakerSectionMinimumWords")) &&
          speakers.contains(QStringLiteral("jcut::projectSpeakerSectionsCore(")) &&
          speakers.contains(QStringLiteral(
              "minimumWords,\n        kTimelineFps")) &&
          sectionCore.contains(QStringLiteral(
              "current.wordCount >= static_cast<std::size_t>(minimumWords)")),
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
  QVERIFY2(keyframes.contains(QStringLiteral("ContinuityAssignmentMode::SectionOnly")),
           "runtime speaker framing must be able to sample only the active "
           "contiguous section assignment before considering clip-level manual "
           "tracks");
  const qsizetype sectionOnlyIndex =
      keyframes.indexOf(QStringLiteral("ContinuityAssignmentMode::SectionOnly"));
  const qsizetype manualSampleIndex =
      keyframes.indexOf(QStringLiteral("manualContinuityTrackSampleForClip(clip, timelineFrame"));
  QVERIFY2(sectionOnlyIndex >= 0 && manualSampleIndex > sectionOnlyIndex,
           "dynamic speaker framing must prefer the active section assignment "
           "and its rotation over the clip-level manual track sample");

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
  QVERIFY2(renderTools.contains(QStringLiteral("persistExportRequestDefaults")) &&
               renderTools.contains(QStringLiteral("m_exportPlaybackSpeed")) &&
               renderTools.contains(QStringLiteral("request.playbackSpeed = std::isfinite(m_exportPlaybackSpeed")) &&
               renderTools.contains(QStringLiteral("persistExportRequestDefaults(request)")) &&
               renderTools.contains(QStringLiteral("persistExportRequestDefaults(baseRequest)")),
           "section export pre-flight speed must persist as the next export "
           "default instead of being discarded with the temporary request");
  const QString renderDecode = readSourceFile(QStringLiteral("render_decode.cpp"));
  QVERIFY2(!renderDecode.isEmpty(), "render_decode.cpp must be readable");
  QVERIFY2(renderDecode.contains(QStringLiteral("sharedHwDevicesForDecoderContexts")) &&
               renderDecode.contains(QStringLiteral("new editor::DecoderContext(path, sharedHwDevices")),
           "blocking export decode fallback must borrow the export AsyncDecoder "
           "hardware-device pool instead of creating private CUDA contexts for "
           "each batch section");
  const QString renderExportAsync = readSourceFile(QStringLiteral("render_export.cpp"));
  QVERIFY2(!renderExportAsync.isEmpty(), "render_export.cpp must be readable");
  QVERIFY2(renderExportAsync.contains(QStringLiteral("exportNeedsAsyncDecode")) &&
               renderExportAsync.contains(QStringLiteral("isImageSequencePath(decodePath)")) &&
               renderExportAsync.contains(QStringLiteral("if (exportNeedsAsyncDecode(orderedClips))")) &&
               renderExportAsync.contains(QStringLiteral("asyncDecoder.get()")),
           "normal video export must not initialize the preview-style async "
           "decode worker pool; keep async export decode limited to image "
           "sequences that require it");
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

  const QString presenter =
      readSourceFile(QStringLiteral("direct_vulkan_preview_presenter.cpp"));
  QVERIFY2(!presenter.isEmpty(),
           "direct_vulkan_preview_presenter.cpp must be readable");
  QVERIFY2(presenter.contains(QStringLiteral("preview_assigned_face_track_ids")) &&
               presenter.contains(QStringLiteral("selected_speaker_assigned_face_track_ids")),
           "preview diagnostics must expose the currently highlighted/restored "
           "FaceDetections track ids");
  QVERIFY2(routes.contains(QStringLiteral("preview_assigned_face_track_ids")) &&
               routes.contains(QStringLiteral("visible_face_track_ids")),
           "playback diagnostics must surface preview face-track selection for "
           "restart regression verification");
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
  QVERIFY2(body.contains(QStringLiteral("editableClip.speakerFramingManualTrackId = trackId")) &&
               body.contains(QStringLiteral("editableClip.speakerFramingManualStreamId = trimmedStreamId")),
           "assigning a face track must persist the manual FaceDetections "
           "track identity on the clip");
  QVERIFY2(body.contains(QStringLiteral("m_deps.scheduleSaveState")) &&
               body.contains(QStringLiteral("m_deps.scheduleSaveState()")),
           "assigning a face track must schedule a project-state save so the "
           "manual selection survives restart");
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
  QVERIFY2(tracks.contains(QStringLiteral("trimmedStreamId == QStringLiteral(\"raw_detection\")")) &&
               tracks.contains(QStringLiteral("QStringLiteral(\"T%1\").arg(trackId)")),
           "section assignment clicks must canonicalize raw-detection track "
           "anchors to their continuity stream id so rotation and track lookup "
           "stay on the same source");
  QVERIFY2(tracks.contains(QStringLiteral("clearAssignedContinuityStreamsCache()")) &&
               tracks.contains(QStringLiteral("prepareClipSpeakerFramingContinuityRuntimeBlocking(*selectedClip)")),
           "section assignment changes must invalidate and immediately warm "
           "speaker-framing continuity caches for the selected clip");
  const QString speakers = readSourceFile(QStringLiteral("speakers_tab.cpp"));
  QVERIFY2(!speakers.isEmpty(), "speakers_tab.cpp must be readable");
  QVERIFY2(tracks.contains(QStringLiteral("flushLoadedTranscriptDocumentForRuntimeNow()")) &&
               speakers.contains(QStringLiteral("invalidateTranscriptJsonCache(transcriptPath)")) &&
               speakers.contains(QStringLiteral("invalidateTranscriptSpeakerProfileCache(transcriptPath)")),
           "contiguous section track and rotation edits must synchronously "
           "publish transcript mutations to the runtime sidecar/cache boundary "
           "before the GPU preview evaluates speaker-framing transforms");
  QVERIFY2(tracks.contains(QStringLiteral("prepareClipSpeakerFramingContinuityRuntimeBlocking(*clip)")) &&
               tracks.contains(QStringLiteral("m_speakerDeps.refreshPreview()")),
           "contiguous section rotation changes must warm the selected clip and "
           "refresh preview immediately so the GPU transform follows the row");
}

void TestDirectVulkanHandoffPipelineContract::
    maskMorphControlsUseWideSliderInputs() {
  const QString inspector = readSourceFile(QStringLiteral("inspector_pane.cpp"));
  const QString maskTab = readSourceFile(QStringLiteral("mask_tab.cpp"));
  QVERIFY2(!inspector.isEmpty(), "inspector_pane.cpp must be readable");
  QVERIFY2(!maskTab.isEmpty(), "mask_tab.cpp must be readable");

  QVERIFY2(inspector.contains(QStringLiteral("#include <QSlider>")) &&
               inspector.contains(QStringLiteral("makePixelsSliderControl")) &&
               inspector.contains(QStringLiteral("slider->setRange(0, qRound(maxValue * 10.0))")),
           "mask morph controls must expose slider input, not only narrow "
           "spin-box arrows");
  QVERIFY2(inspector.contains(QStringLiteral("512.0")) &&
               inspector.contains(QStringLiteral("m_maskDilateSpin = dilateControl.spin")) &&
               inspector.contains(QStringLiteral("shapeForm->addRow(QStringLiteral(\"Dilate\"), dilateControl.row)")) &&
               inspector.contains(QStringLiteral("shapeForm->addRow(QStringLiteral(\"Blur\"), blurControl.row)")),
           "mask morph controls must use wider practical ranges and keep the "
           "spin boxes wired to the saved clip fields");
  QVERIFY2(inspector.contains(QStringLiteral("new QScrollArea(page)")) &&
               inspector.contains(QStringLiteral("scrollArea->setWidgetResizable(true)")) &&
               inspector.contains(QStringLiteral("scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff)")),
           "the mask tab must be vertically scrollable so grading and mask "
           "controls are reachable in a narrow inspector");
  QVERIFY2(maskTab.contains(QStringLiteral(
               "auto setSpin = [](QDoubleSpinBox* spin, double value) {\n"
               "        if (!spin) return;\n"
               "        spin->setValue(value);\n"
               "    };")),
           "mask refresh must allow restored spin-box values to notify paired "
           "sliders while m_updating suppresses clip writes");
}

void TestDirectVulkanHandoffPipelineContract::
    maskDropShadowAndFalloffReachPreviewAndExport() {
  const QString shader = readSourceFile(QStringLiteral("shaders/vulkan/effects.frag"));
  const QString preview = readSourceFile(QStringLiteral("direct_vulkan_preview_window.cpp"));
  const QString previewState = readSourceFile(QStringLiteral("vulkan_preview_surface.cpp"));
  const QString exportBackend =
      readSourceFile(QStringLiteral("offscreen_vulkan_renderer_backend.cpp"));
  QVERIFY2(!shader.isEmpty() && !preview.isEmpty() &&
               !previewState.isEmpty() && !exportBackend.isEmpty(),
           "Vulkan mask rendering sources must be readable");

  QVERIFY2(shader.contains(QStringLiteral("softMaskShadow")) &&
               shader.contains(QStringLiteral("maskShadow")) &&
               shader.contains(QStringLiteral("pc.u_midtones.a")),
           "the Vulkan mask shader must render a soft mask-derived shadow");
  QVERIFY2(previewState.contains(QStringLiteral("status.maskDropShadowEnabled")) &&
               preview.contains(QStringLiteral("drawMaskShadow")) &&
               preview.contains(QStringLiteral("kVulkanEffectModeMaskShadow")),
           "live Vulkan preview must propagate and draw mask-shadow controls");
  QVERIFY2(exportBackend.contains(QStringLiteral("maskDropShadowDraw")) &&
               exportBackend.contains(QStringLiteral("kVulkanEffectModeMaskShadow")) &&
               exportBackend.contains(QStringLiteral("packedMaskFalloff")),
           "Vulkan export must render the same shadow and continuous-alpha falloff as preview");
  QVERIFY2(preview.contains(QStringLiteral("status->maskOpacity")) &&
               exportBackend.contains(QStringLiteral("layer.maskOpacity")),
           "mask opacity must reach both preview and export masked draws");
}

void TestDirectVulkanHandoffPipelineContract::
    startupRestoresSpeechFilterRouting() {
  const QString editor = readSourceFile(QStringLiteral("editor.cpp"));
  const QString transcriptHeader = readSourceFile(QStringLiteral("transcript_tab.h"));
  const QString transcriptSource = readSourceFile(QStringLiteral("transcript_tab.cpp"));
  QVERIFY2(!editor.isEmpty(), "editor.cpp must be readable");
  QVERIFY2(!transcriptHeader.isEmpty(), "transcript_tab.h must be readable");
  QVERIFY2(!transcriptSource.isEmpty(), "transcript_tab.cpp must be readable");

  QVERIFY2(transcriptHeader.contains(QStringLiteral("syncSpeechFilterControlsFromWidgets")) &&
               transcriptSource.contains(QStringLiteral("void TranscriptTab::syncSpeechFilterControlsFromWidgets()")) &&
               transcriptSource.contains(QStringLiteral("currentData().toString() != QStringLiteral(\"none\")")),
           "TranscriptTab must expose an explicit way to synchronize its "
           "speech-filter model from restored widgets when signals are blocked");

  const qsizetype restoreIndex =
      editor.indexOf(QStringLiteral("m_speechFilterEnabled = hasSpeechFilterFadeMode"));
  const qsizetype syncIndex =
      editor.indexOf(QStringLiteral("m_transcriptTab->syncSpeechFilterControlsFromWidgets()"),
                     restoreIndex);
  const qsizetype invalidateIndex =
      editor.indexOf(QStringLiteral("invalidatePlaybackRangeCaches();"), syncIndex);
  const qsizetype playbackRangesIndex =
      editor.indexOf(QStringLiteral("const QVector<ExportRangeSegment> playbackRanges"),
                     invalidateIndex);
  QVERIFY2(restoreIndex >= 0 && syncIndex > restoreIndex,
           "startup state restore must synchronize TranscriptTab after the "
           "speech-filter mode dropdown is restored with signal blockers");
  QVERIFY2(invalidateIndex > syncIndex && playbackRangesIndex > invalidateIndex,
           "startup state restore must invalidate playback range caches before "
           "computing deferred speech-filter playback ranges");
}

void TestDirectVulkanHandoffPipelineContract::
    playbackRangesUseMutationDrivenCache() {
  const QString playback = readSourceFile(QStringLiteral("editor_playback.cpp"));
  const QString timelineWiring =
      readSourceFile(QStringLiteral("editor_editor_pane.cpp"));
  const QString editor = readSourceFile(QStringLiteral("editor.cpp"));
  const QString editorTabs = readSourceFile(QStringLiteral("editor_tabs.cpp"));
  const QString timelineRenderer =
      readSourceFile(QStringLiteral("timeline_renderer.cpp"));
  QVERIFY2(!playback.isEmpty() && !timelineWiring.isEmpty() &&
               !editor.isEmpty() && !editorTabs.isEmpty() &&
               !timelineRenderer.isEmpty(),
           "playback cache and timeline mutation wiring sources must be readable");

  const qsizetype effectiveStart =
      playback.indexOf(QStringLiteral("QVector<ExportRangeSegment> EditorWindow::effectivePlaybackRanges() const"));
  const qsizetype effectiveEnd =
      playback.indexOf(QStringLiteral("QVector<ExportRangeSegment> EditorWindow::applySpeechFilterToExportRanges"),
                       effectiveStart);
  const QString effectiveBody = playback.mid(effectiveStart, effectiveEnd - effectiveStart);
  const qsizetype cacheFastPath =
      effectiveBody.indexOf(QStringLiteral("!m_effectivePlaybackRangesCacheSignature.isEmpty()"));
  const qsizetype signatureBuild =
      effectiveBody.indexOf(QStringLiteral("playbackRangeCacheSignature(false)"));
  QVERIFY2(cacheFastPath >= 0 && signatureBuild > cacheFastPath,
           "effective playback ranges must return the mutation-validated cache "
           "before rebuilding a filesystem-backed clip signature");
  QVERIFY2(effectiveBody.contains(QStringLiteral(
               "if (ranges.isEmpty()) {\n"
               "        m_effectivePlaybackRangesCacheSignature = signature;\n"
               "        m_effectivePlaybackRangesCache.clear();")),
           "an empty playback range result must still become a valid cache entry");
  QVERIFY2(!effectiveBody.contains(QStringLiteral("lastModified()")),
           "the mutation-owned playback cache signature must not poll transcript "
           "file metadata");

  const qsizetype transcriptMutation =
      editorTabs.indexOf(QStringLiteral("&TranscriptTab::transcriptDocumentChanged"));
  const qsizetype runtimePublish = editorTabs.indexOf(
      QStringLiteral("publishTranscriptDocumentToRuntimeCaches("),
      transcriptMutation);
  const qsizetype playbackInvalidation = editorTabs.indexOf(
      QStringLiteral("invalidatePlaybackRangeCaches();"),
      transcriptMutation);
  const qsizetype previewInvalidation = editorTabs.indexOf(
      QStringLiteral("m_preview->invalidateTranscriptOverlayCache("),
      transcriptMutation);
  QVERIFY2(transcriptMutation >= 0 &&
               runtimePublish > transcriptMutation &&
               playbackInvalidation > runtimePublish &&
               previewInvalidation > playbackInvalidation,
           "Transcript editor mutations must publish the live document to the "
           "shared runtime cache before playback and Vulkan preview "
           "invalidation, without waiting for asynchronous disk persistence");

  const qsizetype normalizeStart =
      playback.indexOf(QStringLiteral("QVector<ExportRangeSegment> EditorWindow::effectiveTranscriptNormalizeRanges() const"));
  const qsizetype normalizeEnd =
      playback.indexOf(QStringLiteral("QString EditorWindow::playbackRangeCacheSignature"),
                       normalizeStart);
  const QString normalizeBody = playback.mid(normalizeStart, normalizeEnd - normalizeStart);
  const qsizetype normalizeFastPath =
      normalizeBody.indexOf(QStringLiteral(
          "!m_effectiveTranscriptNormalizeRangesCacheSignature.isEmpty()"));
  const qsizetype normalizeSignatureBuild =
      normalizeBody.indexOf(QStringLiteral("playbackRangeCacheSignature(true"));
  QVERIFY2(normalizeFastPath >= 0 &&
               normalizeSignatureBuild > normalizeFastPath,
      "transcript normalization must use the same cache-before-signature contract");

  for (const QString& callback : {
           QStringLiteral("m_timeline->clipsChanged = [this]()"),
           QStringLiteral("m_timeline->renderSyncMarkersChanged = [this]()"),
           QStringLiteral("m_timeline->exportRangeChanged = [this]()")}) {
    const qsizetype callbackStart = timelineWiring.indexOf(callback);
    const qsizetype invalidate =
        timelineWiring.indexOf(QStringLiteral("invalidatePlaybackRangeCaches();"),
                               callbackStart);
    const qsizetype recompute =
        timelineWiring.indexOf(QStringLiteral("effectivePlaybackRanges();"),
                               callbackStart);
    QVERIFY2(callbackStart >= 0 && invalidate > callbackStart &&
                 recompute > invalidate,
             qPrintable(QStringLiteral(
                 "%1 must invalidate playback ranges before recomputing them")
                            .arg(callback)));
  }

  const qsizetype staleWorkerGuard = editor.indexOf(QStringLiteral(
      "completedGeneration < m_transcriptNormalizeRefreshGeneration"));
  const qsizetype workerCachePublish = editor.indexOf(QStringLiteral(
      "m_effectiveTranscriptNormalizeRangesCacheSignature = completedSignature"));
  QVERIFY2(staleWorkerGuard >= 0 && workerCachePublish > staleWorkerGuard,
           "a stale transcript-normalization worker must be rejected before "
           "it can repopulate a mutation-invalidated cache");
  QVERIFY2(
      timelineRenderer.contains(QStringLiteral(
          "transcriptPathExistsWithBoundedRefresh(transcriptPath)")) &&
          !timelineRenderer.contains(
              QStringLiteral("QFileInfo::exists(transcriptPath)")),
      "timeline painting must use bounded transcript discovery instead of "
      "polling every visible clip path on every repaint");
}

void TestDirectVulkanHandoffPipelineContract::
    speechFilterPassthroughModePersistsAsPassThroughState() {
  const QString editor = readSourceFile(QStringLiteral("editor.cpp"));
  const QString projectState = readSourceFile(QStringLiteral("project_state.cpp"));
  const QString inspector = readSourceFile(QStringLiteral("inspector_pane.cpp"));
  const QString bindings = readSourceFile(QStringLiteral("editor_inspector_bindings.cpp"));
  QVERIFY2(!editor.isEmpty(), "editor.cpp must be readable");
  QVERIFY2(!projectState.isEmpty(), "project_state.cpp must be readable");
  QVERIFY2(!inspector.isEmpty(), "inspector_pane.cpp must be readable");
  QVERIFY2(!bindings.isEmpty(), "editor_inspector_bindings.cpp must be readable");

  QVERIFY2(!projectState.contains(QStringLiteral("root[QStringLiteral(\"speechFilterEnabled\")]")) &&
               projectState.contains(QStringLiteral("root[QStringLiteral(\"speechFilterFadeMode\")]")) &&
               projectState.contains(QStringLiteral(": QStringLiteral(\"none\")")),
           "project state must persist pass-through speech filtering as Passthrough");

  QVERIFY2(editor.contains(QStringLiteral("legacySpeechFilterEnabled")) &&
               editor.contains(QStringLiteral("speechFilterFadeModeValue != QStringLiteral(\"none\")")) &&
               editor.contains(QStringLiteral("AudioEngine::speechFilterFadeModeFromString(")) &&
               editor.contains(QStringLiteral(": QStringLiteral(\"none\")")),
           "state restore must read Passthrough from speechFilterFadeMode");
  QVERIFY2(inspector.contains(QStringLiteral("QStringLiteral(\"Passthrough\"), QStringLiteral(\"none\")")) &&
               inspector.contains(QStringLiteral("SpeechFilterFadeMode::JumpCut")) &&
               inspector.contains(QStringLiteral("SpeechFilterFadeMode::Fade")) &&
               inspector.contains(QStringLiteral("SpeechFilterFadeMode::SmoothStep")) &&
               inspector.contains(QStringLiteral("SpeechFilterFadeMode::SmootherStep")) &&
               inspector.contains(QStringLiteral("SpeechFilterFadeMode::Crossfade")),
           "the speech-filter combo must include Passthrough and the supported transition modes");
  QVERIFY2(bindings.contains(QStringLiteral("mode != QStringLiteral(\"none\")")) &&
               bindings.contains(QStringLiteral("speechFilterFadeModeFromString(mode)")),
           "changing the combo must derive pass-through from Passthrough");
}

void TestDirectVulkanHandoffPipelineContract::
    speechFilterFadeParametersOnlyShowWhenRelevant() {
  const QString bindings = readSourceFile(QStringLiteral("editor_inspector_bindings.cpp"));
  const QString inspector = readSourceFile(QStringLiteral("inspector_pane.cpp"));
  QVERIFY2(!bindings.isEmpty(), "editor_inspector_bindings.cpp must be readable");
  QVERIFY2(!inspector.isEmpty(), "inspector_pane.cpp must be readable");

  QVERIFY2(bindings.contains(QStringLiteral("void EditorWindow::refreshSpeechFilterFadeParameterVisibility()")) &&
               bindings.contains(QStringLiteral("m_speechFilterEnabled &&")) &&
               bindings.contains(QStringLiteral("m_speechFilterFadeMode != AudioEngine::SpeechFilterFadeMode::JumpCut")) &&
               bindings.contains(QStringLiteral("m_speechFilterFadeMode == AudioEngine::SpeechFilterFadeMode::SmoothStep")) &&
               bindings.contains(QStringLiteral("m_speechFilterFadeMode == AudioEngine::SpeechFilterFadeMode::SmootherStep")) &&
               bindings.contains(QStringLiteral("m_speechFilterFadeSamplesSpin")) &&
               bindings.contains(QStringLiteral("m_speechFilterCurveStrengthSpin")) &&
               bindings.contains(QStringLiteral("rowLabel->setVisible(showFadeParameters)")) &&
               bindings.contains(QStringLiteral("rowLabel->setVisible(showCurveParameters)")),
           "fade length controls must be hidden unless "
           "speech filtering is enabled and the selected mode uses fade parameters; "
           "curve strength must only appear for smooth-step modes");
  QVERIFY2(bindings.contains(QStringLiteral("setPlaybackTimingContext(")) &&
               bindings.contains(QStringLiteral("speechFilterPlaybackTimingContext(ranges)")),
           "speech-filter transition controls must feed effective speech ranges "
           "into the explicit preview playback timing context so visual animation "
           "follows transcript play time without relying on global timing state");
  QVERIFY2(inspector.contains(QStringLiteral("speechTimingForm->addRow(QStringLiteral(\"Mode\"), m_speechFilterFadeModeCombo)")) &&
               inspector.contains(QStringLiteral("m_speechFilterFadeModeCombo->addItem(QStringLiteral(\"Passthrough\"), QStringLiteral(\"none\"))")) &&
               inspector.contains(QStringLiteral("createDisclosureSection(settingsContainer, QStringLiteral(\"Speech Filter Audio\")")) &&
               inspector.contains(QStringLiteral("audioTransitionForm->addRow(QStringLiteral(\"Audio Fade\"), m_speechFilterFadeSamplesSpin)")) &&
               inspector.contains(QStringLiteral("audioTransitionForm->addRow(QStringLiteral(\"Curve Strength\"), m_speechFilterCurveStrengthSpin)")) &&
               !inspector.contains(QStringLiteral("audioTransitionForm->addRow(QStringLiteral(\"Audio Transition\"), m_speechFilterRangeCrossfadeCheckBox)")) &&
               inspector.contains(QStringLiteral("createDisclosureSection(settingsContainer, QStringLiteral(\"Frame Transition\")")) &&
               inspector.contains(QStringLiteral("frameTransitionForm->addRow(QStringLiteral(\"Mode\"), m_speechFilterFrameTransitionModeCombo)")) &&
               inspector.contains(QStringLiteral("PlaybackFrameTransitionMode::SmoothStepSpeedThrough")) &&
               inspector.contains(QStringLiteral("PlaybackFrameTransitionMode::SmootherStepSpeedThrough")) &&
               inspector.contains(QStringLiteral("frameTransitionForm->addRow(QStringLiteral(\"Length\"), m_speechFilterFrameCrossfadeFramesSpin)")),
           "speech-filter controls must expose separate audio fade parameters "
	           "and frame crossfade parameters as rows");
}

void TestDirectVulkanHandoffPipelineContract::
    effectsExposeSpeechFilterSynchronizedMotion() {
  const QString inspector = readSourceFile(QStringLiteral("inspector_pane.cpp"));
  const QString header = readSourceFile(QStringLiteral("inspector_pane.h"));
  const QString effects = readSourceFile(QStringLiteral("effects_tab.cpp"));
  const QString editorTabs = readSourceFile(QStringLiteral("editor_tabs.cpp"));
  QVERIFY2(!inspector.isEmpty(), "inspector_pane.cpp must be readable");
  QVERIFY2(!header.isEmpty(), "inspector_pane.h must be readable");
  QVERIFY2(!effects.isEmpty(), "effects_tab.cpp must be readable");
  QVERIFY2(!editorTabs.isEmpty(), "editor_tabs.cpp must be readable");

  QVERIFY2(
      inspector.contains(QStringLiteral("m_effectSpeechSyncCheck")) &&
          inspector.contains(QStringLiteral("Synchronize motion with Speech Filter")) &&
          inspector.contains(QStringLiteral("skipped gaps do not create visible jumps")),
      "Effects tab must expose a clearly labelled speech-filter motion sync "
      "control for moving synthesis patterns");
  QVERIFY2(
      header.contains(QStringLiteral("effectSpeechSyncCheck()")) &&
          editorTabs.contains(QStringLiteral("m_inspectorPane->effectSpeechSyncCheck()")),
      "EffectsTab widget wiring must include the speech-filter motion sync checkbox");
  QVERIFY2(
      effects.contains(QStringLiteral("m_widgets.effectSpeechSyncCheck")) &&
          effects.contains(QStringLiteral("clip->effectSkipAwareTiming")) &&
          effects.contains(QStringLiteral("clip.effectSkipAwareTiming = speechSync")) &&
          effects.contains(QStringLiteral("m_widgets.effectSpeechSyncCheck->setEnabled(false)")) &&
          effects.contains(QStringLiteral(
              "imagePresetCapable && imagePresetActive &&\n"
              "            (steadyIncrease || !progressiveEdgePreset)")) &&
          effects.contains(QStringLiteral("preset != ClipEffectPreset::None")),
      "Effects tab must round-trip the checkbox through the effect-specific "
      "effectSkipAwareTiming render flag and enable it for active visual "
      "effect presets, including transcript-aware steady-increase motion");

  const QString window = readSourceFile(QStringLiteral("direct_vulkan_preview_window.cpp"));
  const QString exportSource = readSourceFile(QStringLiteral("render_export.cpp"));
  const QString offscreen = readSourceFile(QStringLiteral("offscreen_vulkan_renderer_backend.cpp"));
  const QString renderInternal = readSourceFile(QStringLiteral("render_internal.h"));
  const QString surface = readSourceFile(QStringLiteral("vulkan_preview_surface.cpp"));
  QVERIFY2(!window.isEmpty(), "direct_vulkan_preview_window.cpp must be readable");
  QVERIFY2(!exportSource.isEmpty(), "render_export.cpp must be readable");
  QVERIFY2(!offscreen.isEmpty(), "offscreen_vulkan_renderer_backend.cpp must be readable");
  QVERIFY2(!renderInternal.isEmpty(), "render_internal.h must be readable");
  QVERIFY2(!surface.isEmpty(), "vulkan_preview_surface.cpp must be readable");
  QVERIFY2(
      window.contains(QStringLiteral("status ? status->visualTimelineFramePosition : state->currentFramePosition")) &&
          window.contains(QStringLiteral("state->currentFramePosition")) &&
          window.contains(QStringLiteral("clipEffectPlaybackFramePosition(")),
      "direct preview must sample video from the visual speed-through frame "
      "while driving generated effect motion from the raw speech-filter clock");
  QVERIFY2(
      surface.contains(QStringLiteral("const qreal transformFramePosition = frameClocks.visualTimelineFrame")) &&
          surface.contains(QStringLiteral("transformFramePosition,\n            m_interaction.renderSyncMarkers")) &&
          surface.contains(QStringLiteral("evaluateClipSpeakerFramingEnabledAtPosition(clip, transformFramePosition")) &&
          surface.contains(QStringLiteral("evaluateClipSpeakerFramingTargetAtPosition(clip, transformFramePosition")),
      "direct preview must evaluate zoom/transform and speaker framing from "
      "the same projected visual timeline frame used for video sampling");
  QVERIFY2(
      exportSource.contains(QStringLiteral("&frameExportFaceTransformDiagnostics")) &&
          exportSource.contains(QStringLiteral("timelineFramePosition);")) &&
          renderInternal.contains(QStringLiteral("generatedEffectClockTimelineFrame")) &&
          renderInternal.contains(QStringLiteral("frame speed-through can move video across")) &&
          offscreen.contains(QStringLiteral("generatedEffectClockTimelineFrame")) &&
          offscreen.contains(QStringLiteral("const qreal transformClockTimelineFrame = timelineFrame")) &&
          offscreen.contains(QStringLiteral("transformClockTimelineFrame,\n            request.renderSyncMarkers")) &&
          offscreen.contains(QStringLiteral("clipEffectPlaybackFramePosition(foregroundEffectClip, request.clips, generatedEffectClockTimelineFrame")),
      "export must carry a separate effect timeline frame so moving patterns "
      "do not freeze or jump when visual sampling traverses a speech-filter "
      "gap, while transforms stay on the projected visual timeline frame");
}

void TestDirectVulkanHandoffPipelineContract::
    speechFilterFrameCrossfadeIsVisibleInDirectPreview() {
  const QString state = readSourceFile(QStringLiteral("preview_interaction_state.h"));
  const QString surface = readSourceFile(QStringLiteral("vulkan_preview_surface.cpp"));
  const QString window = readSourceFile(QStringLiteral("direct_vulkan_preview_window.cpp"));
  QVERIFY2(!state.isEmpty(), "preview_interaction_state.h must be readable");
  QVERIFY2(!surface.isEmpty(), "vulkan_preview_surface.cpp must be readable");
  QVERIFY2(!window.isEmpty(), "direct_vulkan_preview_window.cpp must be readable");

  QVERIFY2(state.contains(QStringLiteral("frameCrossfadeActive")) &&
               state.contains(QStringLiteral("frameCrossfadeFrame")) &&
               state.contains(QStringLiteral("frameCrossfadeOpacity")),
           "direct preview status must carry a secondary frame-crossfade image "
           "and opacity separately from audio crossfade state");
  QVERIFY2(surface.contains(QStringLiteral("playbackFrameCrossfadeAtTimelineFrame(m_interaction.currentFramePosition")) &&
               surface.contains(QStringLiteral("playbackVisualTimelineFramePosition(m_interaction.currentFramePosition")) &&
               surface.contains(QStringLiteral("requestFramesForSample(")) &&
               surface.contains(QStringLiteral("status.frameCrossfadeActive = true")) &&
               surface.contains(QStringLiteral("status.frameCrossfadeFrame = secondaryFrame")),
           "Vulkan preview must request and attach the speech-filter frame "
           "crossfade target while the playhead is in the outgoing tail");
  QVERIFY2(window.contains(QStringLiteral("#frameCrossfade")) &&
               window.contains(QStringLiteral("secondaryHandoffResult")) &&
               window.contains(QStringLiteral("crossfadePush.opacity")) &&
               window.contains(QStringLiteral("status->frameCrossfadeOpacity")),
           "direct Vulkan preview must draw the secondary speech-filter frame "
           "as a separate sampled image with opacity, not as an export-only blend");

  const QString exportSource = readSourceFile(QStringLiteral("render_export.cpp"));
  QVERIFY2(!exportSource.isEmpty(), "render_export.cpp must be readable");
  const QString playbackTiming =
      readSourceFile(QStringLiteral("playback_timing_context.h"));
  QVERIFY2(!playbackTiming.isEmpty(), "playback_timing_context.h must be readable");
  QVERIFY2(playbackTiming.contains(QStringLiteral("struct PlaybackTimelineFrameClocks")) &&
               playbackTiming.contains(QStringLiteral("playbackTimelineFrameClocks(")) &&
               exportSource.contains(QStringLiteral("playbackTimelineFrameClocks(timelineFramePosition")) &&
               exportSource.contains(QStringLiteral("frameClocks.visualTimelineFrame")) &&
               surface.contains(QStringLiteral("playbackTimelineFrameClocks(m_interaction.currentFramePosition")),
           "export must render the smooth speed-through visual timeline frame, "
           "not only the unwarped speech-filter playhead frame; preview and "
           "export must use the shared clock-domain helper");
  const qsizetype renderCall =
      exportSource.indexOf(QStringLiteral("activeRenderer->renderFrameToOutput(request,"));
  const qsizetype visualFrameArgument =
      exportSource.indexOf(QStringLiteral("frameClocks.visualTimelineFrame,"), renderCall);
  const qsizetype rawTransportArgument =
      exportSource.indexOf(QStringLiteral("frameClocks.transportTimelineFrame,"), visualFrameArgument);
  QVERIFY2(renderCall >= 0 &&
               visualFrameArgument > renderCall &&
               rawTransportArgument > visualFrameArgument,
           "export must pass the same visual timeline frame used by preview "
           "into the compositor while keeping the raw transport frame only for "
           "generated effect phase");
}

void TestDirectVulkanHandoffPipelineContract::
    transcriptTimingEditsInvertDisplayPadding() {
  const QString transcript = readSourceFile(QStringLiteral("transcript_tab.cpp"));
  const QString mutationCore =
      readSourceFile(QStringLiteral("transcript_document_mutation_core.cpp"));
  QVERIFY2(!transcript.isEmpty(), "transcript_tab.cpp must be readable");
  QVERIFY2(!mutationCore.isEmpty(),
           "transcript_document_mutation_core.cpp must be readable");

  QVERIFY2(transcript.contains(QStringLiteral("const double offsetSeconds = transcriptOffsetMs() / 1000.0")) &&
               transcript.contains(QStringLiteral("const double prependSeconds = transcriptPrependMs() / 1000.0")) &&
               transcript.contains(QStringLiteral("const double postpendSeconds = transcriptPostpendMs() / 1000.0")) &&
               transcript.contains(QStringLiteral("? qMax(0.0, seconds - offsetSeconds + prependSeconds)")) &&
               transcript.contains(QStringLiteral(": qMax(0.0, seconds - offsetSeconds - postpendSeconds)")) &&
               transcript.contains(QStringLiteral("patch.startSeconds = rawSeconds")) &&
               transcript.contains(QStringLiteral("patch.endSeconds = rawSeconds")) &&
               transcript.contains(QStringLiteral("jcut::patchTranscriptWord(")) &&
               mutationCore.contains(QStringLiteral(
                   "if (patch.startSeconds && !patch.endSeconds) start = std::min(start, oldEnd)")) &&
               mutationCore.contains(QStringLiteral("end = std::max(start, end)")),
           "transcript source-time edits must invert displayed prepend/postpend "
           "padding before the shared mutation core clamps and saves raw word "
           "timing");
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

void TestDirectVulkanHandoffPipelineContract::
    exportPreviewUsesGpuDoubleBufferOnDedicatedSurface() {
  const QString frameContract =
      readSourceFile(QStringLiteral("core/offscreen_vulkan_frame.h"));
  const QString producer =
      readSourceFile(QStringLiteral("offscreen_vulkan_renderer_backend.cpp"));
  const QString presenter =
      readSourceFile(QStringLiteral("direct_vulkan_preview_window.cpp"));
  const QString imguiPresenter =
      readSourceFile(QStringLiteral("imgui_preview_window.cpp"));
  const QString imguiPresenterHeader =
      readSourceFile(QStringLiteral("imgui_preview_window.h"));
  const QString exportLoop =
      readSourceFile(QStringLiteral("render_export.cpp"));
  const QString exportUi =
      readSourceFile(QStringLiteral("editor_render_tools.cpp"));

  QVERIFY2(frameContract.contains(QStringLiteral("readySemaphoreFd")) &&
               frameContract.contains(QStringLiteral("consumedSemaphoreFd")) &&
    frameContract.contains(QStringLiteral("bufferIndex")) &&
               frameContract.contains(QStringLiteral("producerSessionId")) &&
               frameContract.contains(QStringLiteral("generation")) &&
               frameContract.contains(QStringLiteral("consumptionState")),
           "the GPU preview frame contract must identify both synchronization "
           "directions, generation, and non-blocking host acknowledgment");
  QVERIFY2(producer.contains(QStringLiteral("m_previewSlots.resize(3)")) &&
               producer.contains(QStringLiteral("slot.readySemaphore")) &&
               producer.contains(QStringLiteral("slot.consumedSemaphore")) &&
               producer.contains(QStringLiteral(
                   "VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO")) &&
               producer.contains(QStringLiteral("VK_QUEUE_FAMILY_EXTERNAL")) &&
               producer.contains(QStringLiteral(
                   "all optional preview slots are busy")) &&
               producer.contains(QStringLiteral(
                   "completedGeneration.load")),
           "the export renderer must own exactly three externally synchronized "
           "GPU preview images and drop updates instead of waiting for a busy "
           "optional consumer");
  QVERIFY2(presenter.contains(QStringLiteral("m_gpuExportPreviewSlots")) &&
               presenter.contains(QStringLiteral(
                   "slot.producerSessionId != frame.producerSessionId")) &&
               presenter.contains(QStringLiteral("importExternalFrame(")) &&
               presenter.contains(QStringLiteral("finishPendingCopy(")) &&
               presenter.contains(QStringLiteral("signalConsumed")) &&
               presenter.contains(QStringLiteral(
                   "completedGeneration.store")) &&
               presenter.contains(QStringLiteral(
                   "m_gpuExportPreviewFrames.takeFirst()")) &&
               presenter.contains(QStringLiteral(
                   "m_owner->hasGpuExportPreviewFrames()")),
           "the primary Vulkan presenter must finish each GPU-only import copy "
           "and acknowledge its slot before independent swapchain presentation");
  const qsizetype cleanupDeviceStart = presenter.indexOf(
      QStringLiteral("void DirectVulkanPreviewWindow::cleanupDevice()"));
  const qsizetype ensureReadyStart = presenter.indexOf(
      QStringLiteral("bool DirectVulkanPreviewWindow::ensureVulkanReady()"),
      cleanupDeviceStart);
  const QString cleanupDeviceBody =
      cleanupDeviceStart >= 0 && ensureReadyStart > cleanupDeviceStart
          ? presenter.mid(
                cleanupDeviceStart, ensureReadyStart - cleanupDeviceStart)
          : QString();
  const qsizetype releaseResourcesStart = presenter.indexOf(
      QStringLiteral(
          "void DirectVulkanPreviewRenderer::releaseResources()"));
  const qsizetype releaseDeviceResourcesStart = presenter.indexOf(
      QStringLiteral(
          "void DirectVulkanPreviewRenderer::releaseDeviceResources()"),
      releaseResourcesStart);
  const QString releaseResourcesBody =
      releaseResourcesStart >= 0 &&
              releaseDeviceResourcesStart > releaseResourcesStart
          ? presenter.mid(
                releaseResourcesStart,
                releaseDeviceResourcesStart - releaseResourcesStart)
          : QString();
  QVERIFY2(cleanupDeviceBody.contains(QStringLiteral(
               "m_renderer->releaseDeviceResources()")) &&
               !releaseResourcesBody.contains(QStringLiteral(
                   "destroyGpuExportPreviewResources()")) &&
               !releaseResourcesBody.contains(QStringLiteral(
                   "m_resources.reset()")) &&
               !releaseResourcesBody.contains(QStringLiteral(
                   "m_clipHandoffResources.clear()")),
           "device-level descriptors, imports, and per-clip handoff resources "
           "must survive swapchain resizes");
  const qsizetype clipResourcesStart = presenter.indexOf(
      QStringLiteral(
          "DirectVulkanPreviewRenderer::ClipHandoffResources*"),
      releaseDeviceResourcesStart);
  const QString releaseDeviceResourcesBody =
      releaseDeviceResourcesStart >= 0 &&
              clipResourcesStart > releaseDeviceResourcesStart
          ? presenter.mid(
                releaseDeviceResourcesStart,
                clipResourcesStart - releaseDeviceResourcesStart)
          : QString();
  QVERIFY2(releaseDeviceResourcesBody.contains(QStringLiteral(
               "destroyGpuExportPreviewResources()")) &&
               releaseDeviceResourcesBody.contains(QStringLiteral(
                   "m_resources.reset()")) &&
               releaseDeviceResourcesBody.contains(QStringLiteral(
                   "m_clipHandoffResources.clear()")),
           "device-level Vulkan resources must be retired exactly at device "
           "teardown");
  const qsizetype hideEventStart =
      presenter.indexOf(QStringLiteral("void hideEvent(QHideEvent* event) override"));
  const qsizetype wheelEventStart =
      presenter.indexOf(QStringLiteral("void wheelEvent(QWheelEvent* event) override"),
                        hideEventStart);
  const QString hideEventBody =
      hideEventStart >= 0 && wheelEventStart > hideEventStart
          ? presenter.mid(hideEventStart, wheelEventStart - hideEventStart)
          : QString();
  QVERIFY2(hideEventBody.contains(QStringLiteral("m_swapchainDirty = true")) &&
               !hideEventBody.contains(QStringLiteral("cleanupSwapchain()")),
           "embedded Vulkan export preview windows must not destroy the "
           "swapchain from Qt hide events; QWindowContainer can hide the "
           "native surface while the platform window is already tearing down");
  const QString frameImportContract =
      readSourceFile(QStringLiteral("core/offscreen_vulkan_frame.h"));
  const QString importer =
      readSourceFile(QStringLiteral("vulkan_external_frame_import_core.cpp"));
  QVERIFY2(frameImportContract.contains(QStringLiteral("memoryAllocationSize")) &&
               importer.contains(QStringLiteral(
                   "requirements.size != frame.memoryAllocationSize")) &&
               importer.contains(QStringLiteral(
                   "VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO")) &&
               importer.contains(QStringLiteral(
                   "imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT")),
           "opaque-FD preview imports must preserve the producer allocation "
           "size, memory type, and exact image usage contract");
  QVERIFY2(exportLoop.contains(QStringLiteral("publishGpuPreview")) &&
               !exportLoop.contains(QStringLiteral(
                   "kGpuExportPreviewTargetIntervalMs")) &&
               !exportLoop.contains(QStringLiteral("gpuPreviewDue")) &&
               !exportLoop.contains(QStringLiteral(
                   "(renderedFramesScheduled % 30) == 0")) &&
               producer.contains(QStringLiteral(
                   "publishLastFrameForGpuPreview(gpuPreviewFrame")) &&
               exportLoop.contains(QStringLiteral(
                   "while (!willQueueAsyncGpuFrame")) &&
               exportLoop.contains(QStringLiteral(
                   "releasePreview.releaseGpuPreview = true")),
           "the export loop must publish GPU frames, preserve encoder timestamp "
           "order, and release the preview consumer before renderer teardown");
  QVERIFY2(exportLoop.contains(QStringLiteral(
               "IncrementalRendererSession")) &&
               exportLoop.contains(QStringLiteral(
                   "rendererSession.renderer.get()")) &&
               exportLoop.contains(QStringLiteral(
                   "persistent Vulkan compositor ")) &&
               exportLoop.contains(QStringLiteral(
                   "initialized once for %1 pending chunk(s)")) &&
               producer.contains(QStringLiteral(
                   "clock.timelineSample < clipTimelineStartSamples(clip)")) &&
               producer.contains(QStringLiteral(
                   "clock.timelineSample >= clipTimelineEndSamples(clip)")),
           "incremental export must retain one headless Vulkan compositor "
           "across chunks and only build text for active timeline clips");
  QVERIFY2(!exportUi.contains(QStringLiteral(
               "new ExportVulkanPreviewWidget")) &&
               !exportUi.contains(QStringLiteral(
                   "renderPreviewWidget->setGpuPreviewFrame")) &&
               !exportUi.contains(QStringLiteral("previewWidget")),
           "the Qt export dialog must not embed a second Qt Vulkan render "
           "window; export monitoring belongs to the Dear ImGui Vulkan "
           "surface");
  QVERIFY2(exportUi.contains(QStringLiteral(
               "std::make_unique<ImGuiPreviewWindow>")) &&
               exportUi.contains(QStringLiteral(
                   "presentRenderMonitorFrame")) &&
               exportUi.contains(QStringLiteral(
                   "imguiRenderMonitorPtr->pumpEvents()")) &&
               exportUi.contains(QStringLiteral(
                   "renderMonitorCancelRequested")) &&
               exportUi.contains(QStringLiteral(
                   "effectiveRequest.gpuExportPreviewEnabled = "
                   "imguiRenderMonitor != nullptr")) &&
               imguiPresenterHeader.contains(QStringLiteral(
                   "RenderMonitorStatus")) &&
               imguiPresenter.contains(QStringLiteral(
                   "std::array<RenderMonitorSlot, 3>")) &&
               imguiPresenter.contains(QStringLiteral(
                   "kMonitorSwapchainWaitTimeoutNs")) &&
               !imguiPresenter.contains(QStringLiteral(
                   "vkAcquireNextImageKHR(impl->device,\n"
                   "                                         wd->Swapchain,\n"
                   "                                         UINT64_MAX")) &&
               !imguiPresenter.contains(QStringLiteral(
                   "vkWaitForFences(impl->device, 1, &fd->Fence, VK_TRUE, UINT64_MAX)")) &&
               !imguiPresenter.contains(QStringLiteral(
                   "pumpEvents();\n    if (!isActive())")) &&
               imguiPresenter.contains(QStringLiteral(
                   "VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME")) &&
               imguiPresenter.contains(QStringLiteral(
                   "consumeRenderMonitorExportFrame")) &&
               imguiPresenter.contains(QStringLiteral(
                   "waitForReady.pWaitSemaphores = &slot.ready")) &&
               imguiPresenter.contains(QStringLiteral(
                   "signal.pSignalSemaphores = &slot.consumed")) &&
               imguiPresenter.contains(QStringLiteral(
                   "completedGeneration.store")) &&
               imguiPresenter.contains(QStringLiteral(
                   "importExternalFrame(frame")) &&
               imguiPresenter.contains(QStringLiteral(
                   "ImVec2(0.0f, 0.0f),\n                 ImVec2(1.0f, 1.0f)")) &&
               imguiPresenter.contains(QStringLiteral(
                   "JCut Render Monitor")),
           "the export UI must provide an optional Dear ImGui Vulkan render "
           "monitor that consumes the same synchronized GPU handoff frame "
           "upright and remains a monitor/control surface, not a second "
           "renderer");
}

void TestDirectVulkanHandoffPipelineContract::
    exportRunsOffGuiThreadWhileDedicatedSurfacePresents() {
  const QString source =
      readSourceFile(QStringLiteral("editor_render_tools.cpp"));
  const qsizetype methodBegin = source.indexOf(QStringLiteral(
      "bool EditorWindow::renderTimelineFromOutputRequest"));
  const qsizetype methodEnd = source.indexOf(QStringLiteral(
      "\nvoid EditorWindow::exportVideoForSpeakersOnSelectedClip"), methodBegin);
  const QString method =
      methodBegin >= 0 && methodEnd > methodBegin
          ? source.mid(methodBegin, methodEnd - methodBegin)
          : QString();

  QVERIFY2(!method.isEmpty(),
           "renderTimelineFromOutputRequest must be readable");
  QVERIFY2(method.contains(QStringLiteral("QtConcurrent::run")) &&
               method.contains(QStringLiteral("QFutureWatcher<RenderResult>")) &&
               source.contains(QStringLiteral("ScopedRenderWorkerThreadName")) &&
               source.contains(QStringLiteral("\"jcut-render\"")) &&
               source.contains(QStringLiteral("JCut Render Export")) &&
               method.contains(QStringLiteral("renderEventLoop.exec()")),
           "the export engine must run on a worker while the GUI event loop "
           "continues driving render progress and the Dear ImGui monitor; "
           "the worker must be named for profiler diagnostics");
  QVERIFY2(method.contains(QStringLiteral("Qt::BlockingQueuedConnection")) &&
               method.contains(QStringLiteral("progress.releaseGpuPreview")) &&
               method.contains(QStringLiteral(
                   "if (progress.gpuPreviewFrame.valid)")) &&
               method.contains(QStringLiteral("Qt::QueuedConnection")),
           "preview frame delivery must be non-blocking; only final consumer "
           "release may synchronously marshal before producer teardown");
  QVERIFY2(method.contains(QStringLiteral(
               "effectiveRequest.gpuExportPreviewEnabled =")) &&
               readSourceFile(QStringLiteral("render.h")).contains(
                   QStringLiteral(
                       "bool gpuExportPreviewEnabled = false")) &&
               readSourceFile(QStringLiteral("render_export.cpp")).contains(
                   QStringLiteral(
                       "request.gpuExportPreviewEnabled")),
           "headless and non-preview exports must not publish borrowed GPU "
           "preview slots that have no consumer");
  QVERIFY2(method.contains(QStringLiteral("Qt::QueuedConnection")) &&
               method.contains(QStringLiteral("latestProgress")) &&
               method.contains(QStringLiteral("updateQueued")),
           "ordinary export progress must use one coalesced non-blocking GUI "
           "update instead of back-pressuring the render worker");
  QVERIFY2(method.contains(QStringLiteral("std::atomic_bool")) &&
               method.contains(QStringLiteral("memory_order_relaxed")),
           "render cancellation must remain race-free after progress delivery "
           "is made asynchronous");
  QVERIFY2(!method.contains(QStringLiteral("QCoreApplication::processEvents")),
           "export progress must not re-enter Vulkan presentation through "
           "manual event pumping");
}

void TestDirectVulkanHandoffPipelineContract::
    renderSynchronizationWaitsAreBoundedAndDiagnosable() {
  const QString exportBackend =
      readSourceFile(QStringLiteral("offscreen_vulkan_renderer_backend.cpp"));
  const QString preview =
      readSourceFile(QStringLiteral("direct_vulkan_preview_window.cpp"));
  const QString neutral =
      readSourceFile(QStringLiteral("vulkan_compositor_core.cpp"));
  const QString renderInternal =
      readSourceFile(QStringLiteral("render_internal.h"));
  const QString renderStats =
      readSourceFile(QStringLiteral("render_stats.cpp"));
  const QString exportSource =
      readSourceFile(QStringLiteral("render_export.cpp"));

  QVERIFY2(exportBackend.contains(QStringLiteral(
               "kExportGpuFenceTimeoutNs")) &&
               exportBackend.contains(QStringLiteral(
                   "[vulkan-sync-timeout] stage=export_frame_slot")) &&
               exportBackend.contains(QStringLiteral(
                   "__render_frame_layer_build__")) &&
               exportBackend.contains(QStringLiteral(
                   "__render_frame_decode_wait__")) &&
               exportBackend.contains(QStringLiteral(
                   "__render_frame_mask_resolve__")) &&
               exportBackend.contains(QStringLiteral(
                   "__render_frame_text_inputs__")) &&
               exportBackend.contains(QStringLiteral(
                   "__render_frame_guide_prepare__")) &&
               exportBackend.contains(QStringLiteral(
                   "__render_frame_vulkan_composite_submit__")) &&
               exportBackend.contains(QStringLiteral(
                   "finishLastFrameToNv12*() waits only when the specific slot's output")) &&
               exportBackend.contains(QStringLiteral(
                   "if (!submitActiveSlot()) {\n      return false;\n    }\n    m_commandBufferOpenForConversion = false;\n    pendingSlots->push_back(m_activeSlotIndex);")) &&
               !exportBackend.contains(QStringLiteral(
                   "The color and NV12 attachments are shared across frame slots")) &&
               !exportBackend.contains(QStringLiteral(
                   "vkWaitForFences(m_device, 1, &slot.fence, VK_TRUE, "
                   "UINT64_MAX)")),
           "export frame ownership waits must be bounded and identify the "
           "stalled slot and preview synchronization state; renderFrame must "
           "publish substage timings, and NV12 conversion must submit "
           "asynchronously and defer the slot wait until encoder consumption");
  QVERIFY2(renderInternal.contains(QStringLiteral("qint64* layerPlanMs = nullptr")) &&
               renderInternal.contains(QStringLiteral("qint64* textPrepMs = nullptr")) &&
               renderInternal.contains(QStringLiteral("qint64* guideOverlayMs = nullptr")) &&
               renderInternal.contains(QStringLiteral("qint64* gpuCompositeMs = nullptr")) &&
               renderStats.contains(QStringLiteral("layer_plan_ms")) &&
               renderStats.contains(QStringLiteral("text_prep_ms")) &&
               renderStats.contains(QStringLiteral("guide_overlay_ms")) &&
               renderStats.contains(QStringLiteral("gpu_composite_ms")) &&
               exportSource.contains(QStringLiteral("&frameLayerPlanMs")) &&
               exportSource.contains(QStringLiteral("&frameTextPrepMs")) &&
               exportSource.contains(QStringLiteral("&frameGuideOverlayMs")) &&
               exportSource.contains(QStringLiteral("&frameGpuCompositeMs")) &&
               exportSource.contains(QStringLiteral("pending.frameLayerPlanMs")) &&
               exportSource.contains(QStringLiteral("pending.frameGpuCompositeMs")),
           "renderFrame substage timings must be exposed through the compositor "
           "contract, collected by export, and published in worst-frame "
           "diagnostics instead of remaining backend-only counters");
  QVERIFY2(preview.contains(QStringLiteral(
               "kPreviewGpuWaitTimeoutNs")) &&
               preview.contains(QStringLiteral(
                   "stage=preview_swapchain_acquire")) &&
               !preview.contains(QStringLiteral(
                   "vkWaitForFences(m_device, 1, &frame.inFlightFence, "
                   "VK_TRUE, UINT64_MAX)")),
           "visible presentation waits must time out with an actionable stage");
  QVERIFY2(neutral.contains(QStringLiteral(
               "kHeadlessCompositorWaitTimeoutNs")) &&
               !neutral.contains(QStringLiteral(
                   "numeric_limits<std::uint64_t>::max()")),
           "the neutral headless compositor must not wait indefinitely");
}

void TestDirectVulkanHandoffPipelineContract::
    incrementalExportCheckpointsAndLosslesslyRemuxes() {
  const QString contract = readSourceFile(QStringLiteral("render.h"));
  const QString coreContract =
      readSourceFile(QStringLiteral("render_contract_types.h"));
  const QString implementation =
      readSourceFile(QStringLiteral("render_export.cpp"));
  const QString backend =
      readSourceFile(QStringLiteral("offscreen_vulkan_renderer_backend.cpp"));
  const QString hardwareImportCore =
      readSourceFile(QStringLiteral("vulkan_hardware_frame_import_core.cpp"));
  const QString detectorHandoff =
      readSourceFile(QStringLiteral("vulkan_detector_frame_handoff.cpp"));
  const QString outputTab = readSourceFile(QStringLiteral("output_tab.cpp"));
  const QString inspector =
      readSourceFile(QStringLiteral("inspector_pane_secondary_tabs.cpp"));
  const QString inspectorHeader =
      readSourceFile(QStringLiteral("inspector_pane.h"));
  const QString runtime = readSourceFile(QStringLiteral("render_runtime.cpp"));
  const QString compat = readSourceFile(QStringLiteral("render_qt_compat.cpp"));
  const QString preview =
      readSourceFile(QStringLiteral("direct_vulkan_preview_window.cpp"));
  const QString editor = readSourceFile(QStringLiteral("editor.cpp"));
  const QString projectState =
      readSourceFile(QStringLiteral("project_state.cpp"));

  QVERIFY2(contract.contains(QStringLiteral("bool incrementalExport = true")) &&
               coreContract.contains(QStringLiteral("bool incrementalExport = true")) &&
               contract.contains(
                   QStringLiteral("int incrementalChunkFrames = 900")) &&
               coreContract.contains(
                   QStringLiteral("int incrementalChunkFrames = 900")) &&
               contract.contains(
                   QStringLiteral("incrementalFramesReused")) &&
               contract.contains(
                   QStringLiteral("incrementalCachePath")),
           "encoded exports must enable resumable checkpointing by default "
           "and expose reuse diagnostics");
  QVERIFY2(inspector.contains(QStringLiteral("Incremental chunked render")) &&
               inspector.contains(QStringLiteral("m_incrementalRenderCheckBox->setChecked(true)")) &&
               outputTab.contains(QStringLiteral("request.incrementalExport")) &&
               outputTab.contains(QStringLiteral("m_widgets.incrementalRenderCheckBox->isChecked()")) &&
               runtime.contains(QStringLiteral("qtRequest.incrementalExport = request.incrementalExport")) &&
               compat.contains(QStringLiteral("core.incrementalExport = request.incrementalExport")),
           "the Output tab must expose an explicit chunk/no-chunk control and "
           "carry it through the core and Qt render request contracts");
  QVERIFY2(contract.contains(QStringLiteral("bool instagramSafeAreaGuides = false")) &&
               contract.contains(QStringLiteral("bool alignmentGridGuides = false")) &&
               coreContract.contains(QStringLiteral("bool instagramSafeAreaGuides = false")) &&
               coreContract.contains(QStringLiteral("bool alignmentGridGuides = false")) &&
               inspector.contains(QStringLiteral("Render Instagram 250px safe-area guides")) &&
               inspector.contains(QStringLiteral("Render 3x3 alignment grid")) &&
               inspector.contains(QStringLiteral("Show Instagram 250px safe-area guides")) &&
               inspector.contains(QStringLiteral("Show 3x3 alignment grid")) &&
               inspectorHeader.contains(QStringLiteral("PreviewGuideControls")) &&
               inspectorHeader.contains(QStringLiteral("OutputRenderControls")) &&
               !inspectorHeader.contains(QStringLiteral("previewInstagramSafeAreaGuidesCheckBox()")) &&
               !inspectorHeader.contains(QStringLiteral("previewAlignmentGridGuidesCheckBox()")) &&
               outputTab.contains(QStringLiteral("request.instagramSafeAreaGuides")) &&
               outputTab.contains(QStringLiteral("request.alignmentGridGuides")) &&
               !outputTab.contains(QStringLiteral("setInstagramSafeAreaGuidesVisible")) &&
               !outputTab.contains(QStringLiteral("setAlignmentGridGuidesVisible")) &&
               runtime.contains(QStringLiteral("qtRequest.instagramSafeAreaGuides = request.instagramSafeAreaGuides")) &&
               runtime.contains(QStringLiteral("qtRequest.alignmentGridGuides = request.alignmentGridGuides")) &&
               compat.contains(QStringLiteral("core.instagramSafeAreaGuides = request.instagramSafeAreaGuides")) &&
               compat.contains(QStringLiteral("core.alignmentGridGuides = request.alignmentGridGuides")) &&
               backend.contains(QStringLiteral("placementGuideOverlay(request.outputSize")) &&
               backend.contains(QStringLiteral("if (request.instagramSafeAreaGuides || request.alignmentGridGuides)")) &&
               preview.contains(QStringLiteral("drawOutputPlacementGuides")) &&
               preview.contains(QStringLiteral("state->instagramSafeAreaGuides")) &&
               preview.contains(QStringLiteral("state->alignmentGridGuides")) &&
               editor.contains(QStringLiteral("setInstagramSafeAreaGuidesVisible(previewInstagramSafeAreaGuides)")) &&
               editor.contains(QStringLiteral("setAlignmentGridGuidesVisible(previewAlignmentGridGuides)")) &&
               projectState.contains(QStringLiteral("previewInstagramSafeAreaGuides")) &&
               projectState.contains(QStringLiteral("previewAlignmentGridGuides")),
           "Instagram safe-area and 3x3 alignment guides must be explicit "
           "render-pipeline options: disabled defaults are passthrough, Output "
           "controls carry them into export, Preview controls are independently "
           "grouped, persisted, and drive only the Vulkan preview pass, and export "
           "composites them as an output-space layer");
  QVERIFY2(implementation.contains(QStringLiteral(
               "incrementalRenderSignature")) &&
               implementation.contains(QStringLiteral(
                   "kIncrementalRenderSchema = 4")) &&
               implementation.contains(QStringLiteral(
                   "\"checkpointMode\"")) &&
               implementation.contains(QStringLiteral(
                   "\"encoded-chunk\"")) &&
               implementation.contains(QStringLiteral(
                   "editor::clipToJson(clip)")) &&
               implementation.contains(QStringLiteral(
                   "clipSignature.remove(QStringLiteral("
                   "\"audioSourceLastVerifiedMs\"))")) &&
               implementation.contains(QStringLiteral(
                   "QCryptographicHash::Sha256")),
           "chunk reuse must be keyed by a deterministic render/timeline/media "
           "signature");
  QVERIFY2(!backend.contains(QStringLiteral("cuDestroyExternalMemory")),
           "offscreen Vulkan/CUDA export must not call the CUDA external-memory "
           "destroy entry point from render/chunk workers; imported opaque-FD "
           "allocations are retained with their CUDA device owner and retired by "
           "dropping that owner instead");
  QVERIFY2(!hardwareImportCore.contains(QStringLiteral("cuDestroyExternalMemory")) &&
               !detectorHandoff.contains(QStringLiteral("cuDestroyExternalMemory")),
           "first-party Vulkan/CUDA import helpers must follow the same "
           "driver-safe external-memory lifetime policy and avoid explicit CUDA "
           "external-memory destroy calls from worker paths");
  const qsizetype cudaContextRetirementStart = backend.indexOf(
      QStringLiteral(
          "if (slot.cudaExternalMemory && slot.cudaImportContext != cudaContext)"));
  const qsizetype cudaCachedImportReuseStart = backend.indexOf(
      QStringLiteral(
          "if (slot.cudaExternalMemory && slot.cudaExternalDevicePtr)"),
      cudaContextRetirementStart);
  const QString cudaContextRetirement =
      cudaContextRetirementStart >= 0 && cudaCachedImportReuseStart >= 0
          ? backend.mid(cudaContextRetirementStart,
                        cudaCachedImportReuseStart -
                            cudaContextRetirementStart)
          : QString();
  QVERIFY2(backend.contains(QStringLiteral(
               "AVBufferRef *cudaImportDeviceRef = nullptr")) &&
               backend.contains(QStringLiteral(
                   "av_buffer_ref(framesCtx->device_ref)")) &&
               !cudaContextRetirement.isEmpty() &&
               !cudaContextRetirement.contains(
                   QStringLiteral("cuDestroyExternalMemory")) &&
               backend.contains(QStringLiteral(
                   "av_buffer_unref(&slot.cudaImportDeviceRef)")),
           "incremental Vulkan/CUDA export must retain the CUDA device while "
           "an external-memory import is cached and retire a stale import by "
           "releasing that owner instead of destroying it from a new context");
  QVERIFY2(implementation.contains(QStringLiteral("QSaveFile")) &&
               implementation.contains(QStringLiteral("manifest.json")) &&
               implementation.contains(QStringLiteral(
                   "completed.insert(chunk.index)")) &&
               implementation.contains(QStringLiteral(
                   "reusableChunksFromManifest")) &&
               implementation.contains(QStringLiteral(
                   "hasCompleteIncrementalEncodedChunk")) &&
               implementation.contains(QStringLiteral(
                   "encodedVideoFrameCount")) &&
               implementation.contains(QStringLiteral(
                   "incrementalChunkAttemptPath")) &&
               implementation.contains(QStringLiteral(
                   "publishIncrementalEncodedChunk")) &&
               implementation.contains(QStringLiteral(
                   "writeIncrementalChunkFailureDiagnostic")) &&
               implementation.contains(QStringLiteral(
                   "lastFailure")) &&
               implementation.contains(QStringLiteral(
                   "chunkRequest.outputPath = attemptPath")) &&
               implementation.contains(QStringLiteral(
                   "std::rename(attemptName.constData(), chunkName.constData())")),
           "an encoded chunk must become reusable only after the chunk file "
           "exists, has the expected video frame count, has been atomically "
           "published from a temporary attempt path, and its manifest is "
           "atomically checkpointed; failed attempts must leave diagnostics "
           "without poisoning the reusable chunk filename");
  QVERIFY2(implementation.contains(QStringLiteral(
               "progress.elapsedMs = totalTimer.elapsed()")) &&
               implementation.contains(QStringLiteral(
                   "progress.framesCompleted - reusedFrames")) &&
               implementation.contains(QStringLiteral(
                   "totalFrames - progress.framesCompleted")),
           "incremental progress must calculate throughput and ETA from the "
           "global render clock and frames actually rendered in this run");
  QVERIFY2(implementation.contains(QStringLiteral(
               "chunkRequest.exportRanges = chunk.ranges")) &&
               implementation.contains(QStringLiteral(
                   "chunkRequest.incrementalExport = false")) &&
               implementation.contains(QStringLiteral(
                   "chunkRequest.losslessIntermediateAudio = true")) &&
               implementation.contains(QStringLiteral(
                   "chunkRequest.createVideoFromImageSequence = false")) &&
               implementation.contains(QStringLiteral(
                   "chunkRequest.imageSequenceFormat.clear()")),
           "every chunk must reuse the canonical renderer with unchanged "
           "export-range boundaries, encoded GPU/NVENC video checkpoints, "
           "lossless intermediate audio, and no CPU image sequence");
  QVERIFY2(implementation.contains(QStringLiteral(
               "QStringLiteral(\"concat\")")) &&
               !implementation.contains(QStringLiteral(
                   "QStringLiteral(\"frames.txt\")")) &&
               implementation.contains(QStringLiteral(
                   "QStringLiteral(\"0:v:0\")")) &&
               implementation.contains(QStringLiteral(
                   "QStringLiteral(\"-c:v\"), QStringLiteral(\"copy\")")) &&
               implementation.contains(QStringLiteral(
                   "QStringLiteral(\"-c:a\")")) &&
               implementation.contains(QStringLiteral(
                   "QStringLiteral(\".assembling.\")")) &&
               implementation.contains(QStringLiteral(
                   "std::rename(assemblingName.constData(), "
                   "outputName.constData())")),
           "final assembly must stream-copy definitive encoded chunks into a "
           "temporary output before atomically publishing it");
}

void TestDirectVulkanHandoffPipelineContract::
    outputTabClearsOnlyResolvedIncrementalRenderCacheRoot() {
  const QString contract = readSourceFile(QStringLiteral("render.h"));
  const QString exportSource = readSourceFile(QStringLiteral("render_export.cpp"));
  const QString outputTab = readSourceFile(QStringLiteral("output_tab.cpp"));
  const QString inspector = readSourceFile(QStringLiteral("inspector_pane_secondary_tabs.cpp"));

  QVERIFY2(contract.contains(QStringLiteral("incrementalRenderCacheRootForOutputPath")) &&
               exportSource.contains(QStringLiteral("incrementalRenderCacheRootForOutputPath(request.outputPath)")),
           "the Output tab and export must share the same incremental render cache root contract");
  QVERIFY2(inspector.contains(QStringLiteral("Clear Render Cache")) &&
               outputTab.contains(QStringLiteral("onClearRenderCacheClicked")) &&
               outputTab.contains(QStringLiteral("isSafeRenderCacheRoot")) &&
               outputTab.contains(QStringLiteral(".jcut-render-cache")) &&
               outputTab.contains(QStringLiteral("removeRecursively")),
           "the Output tab must expose guarded clearing of only the resolved .jcut-render-cache directory");
  QVERIFY2(outputTab.contains(QStringLiteral("QMessageBox::question")) &&
               outputTab.contains(QStringLiteral("m_widgets.clearRenderCacheButton->setEnabled(exists)")),
           "cache clearing must require confirmation and stay disabled when no cache exists");
}

void TestDirectVulkanHandoffPipelineContract::
    exportCompositionNeverPublishesPartialLayers() {
  const QString backend =
      readSourceFile(QStringLiteral("offscreen_vulkan_renderer_backend.cpp"));
  const QString decode = readSourceFile(QStringLiteral("render_decode.cpp"));
  const QString exportSource = readSourceFile(QStringLiteral("render_export.cpp"));
  const QString effects =
      readSourceFile(QStringLiteral("editor_shared_effects.cpp"));
  const QString titleMesh =
      readSourceFile(QStringLiteral("title_mesh_extrusion.cpp"));
  const QString textRenderer =
      readSourceFile(QStringLiteral("vulkan_text_renderer.cpp"));
  const QString vulkanResources =
      readSourceFile(QStringLiteral("vulkan_resources.cpp"));

  QVERIFY2(backend.contains(QStringLiteral(
               "rawClipMaskBuffer(matteOwner, frame)")) &&
               exportSource.contains(QStringLiteral(
                   "renderedFrame.failureReason.trimmed()")) &&
               exportSource.contains(QStringLiteral(
                   "Failed to render Vulkan timeline frame %1: %2")) &&
               backend.contains(QStringLiteral(
                   "activeTextFrameSlot")) &&
               backend.contains(QStringLiteral(
                   "m_transcriptTextRenderer->beginFrameUploads(activeTextFrameSlot")) &&
               backend.contains(QStringLiteral(
                   "m_speakerTextRenderer->beginFrameUploads(activeTextFrameSlot")) &&
               textRenderer.contains(QStringLiteral(
                   "resources->lastError().trimmed()")) &&
               textRenderer.contains(QStringLiteral(
                   "text_atlas_upload_failed: %1")) &&
               vulkanResources.contains(QStringLiteral(
                   "staging_upload_exceeds_slot")) &&
               vulkanResources.contains(QStringLiteral(
                   "overlay_staging_write_failed")) &&
               backend.contains(QStringLiteral(
                   "rawClipMaskBufferBlocking(matteOwner, frame)")) &&
               backend.contains(QStringLiteral(
                   "vulkanMaskCorrectionStorageData")) &&
               backend.contains(QStringLiteral(
                   "VK_DESCRIPTOR_TYPE_STORAGE_BUFFER")) &&
               !backend.contains(QStringLiteral(
                   "applyCorrectionPolygonsToMaskBuffer")) &&
               !backend.contains(QStringLiteral(
                   "rawClipMaskImageBlocking(matteOwner, frame)")) &&
               !backend.contains(QStringLiteral(
                   "rgbaMaskImageForUpload")) &&
               backend.contains(QStringLiteral(
                   "VK_FORMAT_R8_UNORM")) &&
               backend.contains(QStringLiteral(
                   "writeImageBufferToStagingTopLeft(maskUpload")) &&
               decode.contains(QStringLiteral(
                   "enqueueRenderMaskLookahead")) &&
               decode.contains(QStringLiteral(
                   "prewarmRenderMaskSegment")) &&
               decode.contains(QStringLiteral(
                   "kMinimumExportMaskLookaheadFrames")) &&
               decode.contains(QStringLiteral(
                   "prefetchRenderableClipMaskBuffersAtTimelinePosition")) &&
               effects.contains(QStringLiteral(
                   "visualClipActiveAtTimelineClock")) &&
               effects.contains(QStringLiteral(
                   "clipUsesRenderableSidecarMask")) &&
               effects.contains(QStringLiteral(
                   "cache.loadCompleted.wait(lock")) &&
               effects.contains(QStringLiteral(
                   "storeRawMaskDecodeResult")) &&
               effects.contains(QStringLiteral(
                   "2ull * 1024ull * 1024ull * 1024ull")),
           "export masks must use the shared raw-mask cache with export "
           "lookahead, block only on a missing definitive frame, and stage "
           "cached Gray8 masks without expanding them to RGBA first");
  QVERIFY2(backend.contains(QStringLiteral("QStringList subtitleFailures")) &&
               backend.contains(QStringLiteral("&subtitleFailures")) &&
               backend.contains(QStringLiteral(
                   "Vulkan export refused to drop subtitle overlay(s)")) &&
               backend.contains(QStringLiteral(
                   "transcript text renderer is unavailable")) &&
               backend.contains(QStringLiteral(
                   "m_transcriptTextRenderer->lastFailureReason()")) &&
               backend.contains(QStringLiteral(
                   "const bool transcriptDrawn")) &&
               backend.contains(QStringLiteral(
                   "clip %1 has transcript overlay enabled but no active transcript path")) &&
               backend.contains(QStringLiteral(
                   "has no readable sections")) &&
               !backend.contains(QStringLiteral(
                   "prepareTranscriptOverlayAtlas(\n"
                   "                m_commandBuffer, m_outputSize, text.clip, text.layout,\n"
                   "                text.outputRect, text.speakerTitle)) {\n"
                   "          continue;")),
           "export subtitles must be definitive: an expected subtitle overlay "
           "must be drawn or fail the frame with a diagnostic, never silently "
           "continued past");
  QVERIFY2(backend.contains(QStringLiteral(
               "Vulkan export refused an incomplete composition")) &&
               backend.contains(QStringLiteral(
                   "frameContext.frameFailureReason = "
                   "&output->failureReason")) &&
               backend.contains(QStringLiteral(
                   "if (!output->failureReason.isEmpty())")),
           "a missing eligible layer must fail the output frame rather than "
           "publishing a stale or partial Vulkan composition");
  QVERIFY2(decode.contains(QStringLiteral(
               "Export is definitive, not best-effort")) &&
               decode.contains(QStringLiteral("delete failedDecoder")) &&
               decode.contains(QStringLiteral(
                   "return retryDecoder->decodeFrame(frameNumber)")),
           "a transient decoder failure must receive one clean reopen retry "
           "before the render is stopped");
  QVERIFY2(backend.contains(QStringLiteral(
               "const bool titleClip = clip.mediaType == ClipMediaType::Title")) &&
               backend.contains(QStringLiteral(
                   "const TimelineClip &activeRangeClip = titleClip ? clip : timingSource")) &&
               backend.contains(QStringLiteral(
                   "titleClip ? clip : clipWithResolvedTimingOwner")) &&
               backend.contains(QStringLiteral(
                   "if (titleClip)")),
           "generated speaker-title clips must be evaluated against their own "
           "timeline interval and effects, not the linked media source; "
           "otherwise the first animated title can persist and later titles "
           "can be skipped in export");
  QVERIFY2(titleMesh.contains(QStringLiteral("cachedFontData")) &&
               titleMesh.contains(QStringLiteral("FT_New_Memory_Face")) &&
               titleMesh.contains(QStringLiteral("QHash<QString, QByteArray> cache")) &&
               !titleMesh.contains(QStringLiteral("FT_New_Face(library, fontPath")),
           "3D title mesh generation must cache resolved font bytes and build "
           "FreeType memory faces instead of reopening the font file during "
           "render-frame composition");
}

QTEST_MAIN(TestDirectVulkanHandoffPipelineContract)
#include "test_direct_vulkan_handoff_pipeline_contract.moc"
