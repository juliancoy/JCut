# Preview Render Pipeline

## Purpose

This document is the concrete implementation map for the interactive preview render pipeline in J-Cut.

Use it together with:

- `TIME.md` for temporal domain truth
- `scheduling.md` for decode/prefetch policy
- `synchronization.md` for ownership, threading, and failure contracts

This file answers a different question: when a preview frame is requested, which code, shaders, Vulkan objects, and decoder objects participate before pixels reach the swapchain?

The scope is the current direct Vulkan preview path. OpenGL-era files still exist in the tree, but they are not the authoritative playback path.

## Canonical Runtime Path

At a high level:

1. `EditorWindow` owns the playback clock.
2. `VulkanPreviewSurface` maps the timeline sample to source-frame requests.
3. `TimelineCache` and `AsyncDecoder` produce a `FrameHandle`.
4. `VulkanPreviewSurface` builds `VulkanPreviewClipFrameStatus`.
5. `DirectVulkanPreviewPresenter` renders those statuses into the swapchain.

Primary code entry points:

- `editor_playback.cpp`
- `vulkan_preview_surface.cpp`
- `timeline_cache.cpp`
- `async_decoder.cpp`
- `direct_vulkan_preview_backend.cpp`
- `direct_vulkan_frame_handoff_pipeline.cpp`
- `vulkan_pipeline.cpp`
- `vulkan_text_renderer.cpp`
- `vulkan_audio_tab.cpp`

## Stage Inventory

These are the stages exposed to diagnostics and represented in the current pipeline implementation.

### 0. Clock Update

Owner:

- `EditorWindow`

Main code:

- `EditorWindow::advanceFrame()`
- `EditorWindow::setCurrentPlaybackSample(...)`

Inputs:

- Audio clock or timeline timer
- Playback speed
- Pitch-preserving audio readiness

Outputs:

- Authoritative current playback sample

REST/debug counters:

- `clock_update`
- `playback_sample_apply`

### 1. Timeline Input

Owner:

- `VulkanPreviewSurface`

Main code:

- `VulkanPreviewSurface::setCurrentPlaybackSample(...)`
- `VulkanPreviewSurface::requestFramesForCurrentPosition()`

Inputs:

- Current timeline sample
- Active clip list

Outputs:

- Active clip iteration basis for visible decode

REST/debug counter:

- `timeline_input`

### 2. Source Mapping

Owner:

- `VulkanPreviewSurface`

Main code:

- `VulkanPreviewSurface::sourceFrameForSample(...)`
- `sourceFrameForClipAtTimelineSample(...)`

Inputs:

- Timeline sample
- Clip trims
- Playback rate
- Source FPS
- Render sync markers

Outputs:

- Requested source frame per active clip

REST/debug counter:

- `source_mapping`

### 3. Visible Request

Owner:

- `VulkanPreviewSurface`

Main code:

- `VulkanPreviewSurface::requestFramesForCurrentPosition()`
- `VulkanPreviewSurface::preparePlaybackAdvanceSample(...)`
- `evaluatePreviewVisibleRequest(...)`

Inputs:

- Exact/displayable cache state
- Pending visible request state
- Visible backlog limit
- Lookahead window

Outputs:

- Current visible request
- Lookahead visible requests

Important contract:

- Current exact requests may still dispatch when lookahead backlog is saturated.
- Approximate/displayable frames do not suppress exact current-frame requests.

REST/debug counter:

- `visible_request`

### 4. Cache Lookup

Owner:

- `TimelineCache`

Main code:

- `TimelineCache::requestFrame(...)`
- `TimelineCache::hasDisplayableFrameForPreview(...)`
- `TimelineCache::hasExactFrameForPreview(...)`
- `ClipCache`
- `PlaybackBuffer`

Outputs:

- Exact cached frame, approximate cached frame, or decode request

REST/debug counter:

- `cache_lookup`

### 5. Decoder Output

Owner:

- `AsyncDecoder`

Main code:

- `AsyncDecoder::requestFrame(...)`
- `AsyncDecoder::runLane(...)`
- `DecoderContext::decodeThroughFrame(...)`

Core decoder objects:

- `AsyncDecoder`
- lane-local `DecoderContext`
- shared FFmpeg HW device refs
- `FrameHandle`

Outputs:

- Hardware frame
- External Vulkan frame
- CPU image frame, which is rejected for visible direct Vulkan playback

REST/debug counter:

- `decoder_output`

### 6. Frame Selection

Owner:

- `VulkanPreviewSurface`

Main code:

- `VulkanPreviewSurface::refreshVulkanFrameStatuses()`
- `selectPreviewFrame(...)`
- `previewMaxPlaybackStaleFrameDelta(...)`
- `previewFrameIsTooStaleForPlayback(...)`

Outputs:

- `VulkanPreviewClipFrameStatus`

Important contract:

- Bounded approximate playback is allowed.
- Too-stale approximate frames are diagnosed and rejected rather than silently treated as current.

REST/debug counter:

- `frame_selection`

### 7. Corrections / Mask

Owner:

- `VulkanPreviewSurface`

Main code:

- `refreshVulkanFrameStatuses()`
- effect/transform evaluation helpers

Outputs:

- Mask-related fields in `VulkanPreviewClipFrameStatus`

Current state:

- Correction mask state is diagnosed and propagated through status.
- Direct Vulkan composition currently reports unsupported correction-mask cases rather than using a dedicated mask shader in the main hot path.

REST/debug counter:

- `corrections_mask`

### 8. Effects Evaluation

Owner:

- `VulkanPreviewSurface`

Main code:

- `evaluateEffectiveVisualEffectsAtPosition(...)`

Outputs:

- Brightness
- Contrast
- Saturation
- Opacity
- Lift/gamma/gain style shadow/midtone/highlight values
- Curve LUT usage flags

REST/debug counter:

- `effects_eval`

### 9. Grading Shader Output

Owner:

- `DirectVulkanPreviewRenderer`

Main code:

- `VulkanPipeline::bindAndDraw(...)`

Outputs:

- Fragment-shaded clip image

REST/debug counter:

- `grading_shader`

### 10. Transform

Owner:

- `VulkanPreviewSurface` and `DirectVulkanPreviewRenderer`

Main code:

- `evaluateClipRenderTransformAtPosition(...)`
- `PreviewViewTransform`
- `PreviewViewTransform::clipGeometry(...)`
- `mvpForVulkanClipTransform(...)`

Outputs:

- MVP matrix
- Scissor
- Composite target/fitted rects
- Speaker-framing-adjusted geometry

REST/debug counter:

- `transform`

### 11. Text Preparation

Owner:

- `DirectVulkanPreviewRenderer`

Main code:

- `VulkanTextRenderer::prepareTranscriptOverlayAtlas(...)`
- `VulkanTextRenderer::prepareSpeakerLabelAtlas(...)`

Outputs:

- Prepared glyph atlas texture uploads and layout caches for:
  - transcript subtitles
  - current speaker label
  - temporal debug overlay

REST/debug counter:

- `text_prep`

### 12. Overlay Preparation

Owner:

- `VulkanPreviewSurface`

Main code:

- `VulkanPreviewSurface::refreshFacestreamOverlays()`
- `requestFacestreamOverlaySnapshotAsync(...)`
- `applyFacestreamOverlaySnapshot(...)`

Outputs:

- `facedetectionsOverlays`
- `rawDetectionOverlays`

Important contract:

- Overlay preparation must not block visible video.
- Worker-produced snapshots are keyed and stale results are dropped.

REST/debug counter:

- `overlay_prep`

### 13. Hardware Handoff

Owner:

- `DirectVulkanFrameHandoffPipeline`

Main code:

- `DirectVulkanFrameHandoffPipeline::record(...)`
- `jcut::vulkan_detector::VulkanDetectorFrameHandoff`

Inputs:

- `FrameHandle` from `VulkanPreviewClipFrameStatus`
- Per-clip `VulkanResources`
- Current Vulkan command buffer

Outputs:

- Sampleable Vulkan image and descriptor set

Modes:

- hardware-direct upload/copy
- external memory import

REST/debug counter:

- `gpu_handoff`

### 14. Vulkan Composite / Command Recording

Owner:

- `DirectVulkanPreviewRenderer`

Main code:

- `DirectVulkanPreviewRenderer::startNextFrame()`
- `vkCmdBeginRenderPass(...)`
- `VulkanPipeline::bindAndDraw(...)`
- `VulkanTextRenderer::drawTranscriptOverlay(...)`
- `VulkanTextRenderer::drawSpeakerLabel(...)`
- box-outline helpers using clear operations

Outputs:

- Recorded graphics commands for the swapchain render pass

REST/debug counter:

- `command_recording`

### 15. Presentation

Owner:

- `DirectVulkanPreviewWindow`

Main code:

- `markPresented()`
- `frameReady()`

Outputs:

- Presented swapchain image
- `presentedSourceFrame`

REST/debug counter:

- `presentation`

## Vulkan Object Inventory

This section lists the concrete Vulkan objects participating in the direct preview path.

### Swapchain / Frame Context

Owned by Qt `QVulkanWindow` integration:

- `VkRenderPass` from `defaultRenderPass()`
- `VkFramebuffer` from `currentFramebuffer()`
- `VkCommandBuffer` from `currentCommandBuffer()`
- graphics queue and queue family from the preview window

These are the outer frame objects used by `DirectVulkanPreviewRenderer::startNextFrame()`.

### Shared Sampled-Image Resources

Class:

- `VulkanResources`

Key Vulkan objects:

- `VkSampler m_sampler`
- `VkDescriptorSetLayout m_descriptorSetLayout`
- `VkDescriptorPool m_descriptorPool`
- `VkDescriptorSet[3] m_descriptorSets`
- `VkImage m_textureImage`
- `VkImageView m_textureView`
- `VkDeviceMemory m_textureMemory`
- `VkImage m_curveLutImage`
- `VkImageView m_curveLutView`
- `VkDeviceMemory m_curveLutMemory`
- `VkBuffer m_stagingBuffer`
- `VkDeviceMemory m_stagingMemory`

Usage:

- Main clip texture sampling
- Curve LUT sampling
- Atlas texture uploads
- Optional playback status overlay texture path

Descriptor contract:

- Descriptor sets are triple-buffered (`kDescriptorSetCount = 3`)

### Main Clip Graphics Pipeline

Class:

- `VulkanPipeline`

Key Vulkan objects:

- `VkPipelineLayout m_pipelineLayout`
- `VkPipeline m_pipeline`
- `VkShaderModule m_vertShader`
- `VkShaderModule m_fragShader`

Draw model:

- triangle strip
- dynamic viewport
- dynamic scissor
- alpha blending enabled
- push constants carry MVP plus grading/effect parameters

### Text Graphics Pipelines

Classes:

- `VulkanTextPipeline`
- `VulkanTextRenderer`

Key Vulkan objects per renderer instance:

- `VkPipelineLayout`
- `VkPipeline`
- `VkShaderModule` vertex/fragment pair
- atlas-side `VulkanResources`

Live renderer instances in the direct preview backend:

- transcript text renderer
- current-speaker label renderer
- temporal debug text renderer

### Per-Clip Handoff Resources

Owned by:

- `DirectVulkanPreviewRenderer::ClipHandoffResources`

Key objects:

- one `VulkanResources` per clip
- one `DirectVulkanFrameHandoffPipeline` per clip
- one underlying `VulkanDetectorFrameHandoff`

Lifetime rule:

- resources for inactive clips are retired, then held beyond descriptor-ring depth before destruction

### Audio View Pipelines

Class:

- `jcut::VulkanAudioTab`

Key Vulkan objects:

- `VkDescriptorSetLayout`
- `VkDescriptorPool`
- `VkDescriptorSet`
- `VkPipelineLayout m_pipelineLayout`
- `VkPipelineLayout m_computePipelineLayout`
- one graphics pipeline for waveform/spectrum presentation
- multiple compute pipelines for waveform and spectrum preprocessing

This path is active only when preview `viewMode == Audio`.

## Shader Inventory

This is the current shader inventory relevant to interactive preview.

### Hot-Path Visual Preview Shaders

Used by `VulkanPipeline`:

- `shaders/vulkan/effects.vert`
- `shaders/vulkan/effects.frag`

Used by `VulkanTextPipeline`:

- `shaders/vulkan/text.vert`
- `shaders/vulkan/text.frag`

These are the primary direct-Vulkan playback shaders for video and text.

### Audio Preview Shaders

Used by `VulkanAudioTab`:

- `shaders/vulkan/audio_waveform.vert`
- `shaders/vulkan/audio_waveform.frag`
- `shaders/vulkan/audio_waveform_process.comp`
- `shaders/vulkan/audio_spectrum_loiacono.comp`
- `shaders/vulkan/audio_spectrum_goertzel.comp`
- `shaders/vulkan/audio_spectrum_fft.comp`
- `shaders/vulkan/audio_spectrum_normalize.comp`
- `shaders/vulkan/audio_spectrum_history.comp`
- `shaders/vulkan/audio_spectrum_loiacono_tile.comp`
- `shaders/vulkan/audio_spectrum_goertzel_tile.comp`
- `shaders/vulkan/audio_spectrum_fft_tile.comp`
- `shaders/vulkan/audio_spectrum_tile_normalize.comp`
- `shaders/vulkan/audio_spectrogram_history_normalize.comp`

### Present In Repository But Not The Main Direct-Preview Video Hot Path

These shader files exist, but they are not the primary clip-composite shaders used by the current direct Vulkan playback path documented above:

- `shaders/vulkan/mask.vert`
- `shaders/vulkan/mask.frag`
- `shaders/vulkan/nv12.vert`
- `shaders/vulkan/nv12_y.frag`
- `shaders/vulkan/nv12_uv.frag`
- `shaders/vulkan/yuv420p.comp`
- `shaders/vulkan/yuv420p_u.frag`
- `shaders/vulkan/yuv420p_v.frag`
- `shaders/vulkan/nv12_buffer_to_rgba.comp`
- `shaders/vulkan/face_preprocess.comp`
- `shaders/vulkan/face_infer_heuristic.comp`
- `shaders/vulkan/scrfd_preprocess.comp`

If one of these becomes part of the live interactive preview hot path, this document should be updated immediately.

## What Draws Without Custom Shaders

Not every visible preview element is emitted through a dedicated graphics pipeline.

These are currently produced with clear/rect helper operations on the active command buffer:

- composite canvas clear
- canvas border
- selected-clip outline
- face-stream track boxes
- raw face-detection boxes
- speaker target box

That means these overlays are still part of the preview render pipeline, but they are not backed by their own named shader modules in the current implementation.

## Decoder And Cache Inventory

### AsyncDecoder

Primary class:

- `editor::AsyncDecoder`

Key responsibilities:

- maintain worker lanes
- own shared FFmpeg hardware-device references
- schedule visible/prefetch/preload requests
- return `FrameHandle`

Important output types:

- hardware frame
- external Vulkan frame
- CPU image frame

### TimelineCache

Primary class:

- `editor::TimelineCache`

Key responsibilities:

- coalesce visible requests
- hold `ClipCache`
- hold playback buffer
- provide exact/displayable lookup decisions
- expose visible decode diagnostics

### Frame Handle

Bridge object:

- `FrameHandle`

It is the media payload contract crossing from decode/cache into preview selection and then into Vulkan handoff.

## Frame Build Order Inside `startNextFrame()`

The render-thread order in `DirectVulkanPreviewRenderer::startNextFrame()` is:

1. Acquire current swapchain command buffer and render-pass context.
2. Advance retired per-clip handoff resources.
3. Build per-clip handoff results before the render pass.
4. Upload per-clip curve LUT textures before draw.
5. Prepare transcript atlases.
6. Prepare speaker-label atlas.
7. Prepare temporal-debug atlas.
8. Begin the swapchain render pass.
9. If audio view is active, record audio preview commands and present.
10. Otherwise clear the composite canvas.
11. For each active clip:
    - compute fitted geometry and transform
    - hand off hardware/external frame to a sampleable Vulkan image
    - bind `VulkanPipeline`
    - push MVP and grading data
    - draw textured quad
    - draw transcript text if prepared
12. Draw current speaker label.
13. Draw temporal debug label.
14. Draw playback-status overlay if active.
15. Draw face-track and raw-detection boxes.
16. Draw selection/target outlines.
17. End render pass.
18. Signal present readiness and schedule the next update if playback continues.

This ordering is important. The contract tests already assert several of these sequencing requirements.

## REST / Diagnostics Surface

Current authoritative runtime diagnostics:

- `GET /pipeline`
- `GET /pipeline?verbose=1`
- `GET /profile`

Current playback stage counters are exposed under:

- `/profile.playback_pipeline_stages`
- `/pipeline.playback_pipeline_stages`

Current stage keys:

- `clock_update`
- `playback_sample_apply`
- `timeline_input`
- `source_mapping`
- `visible_request`
- `cache_lookup`
- `decoder_output`
- `frame_selection`
- `corrections_mask`
- `effects_eval`
- `grading_shader`
- `transform`
- `overlay_prep`
- `text_prep`
- `gpu_handoff`
- `command_recording`
- `presentation`

Each stage reports:

- `attempts`
- `successes`
- `source_unavailable`
- `last_state`
- `last_reason`
- `last_updated_ms`

## Non-Goals / Explicit Exclusions

The current direct preview path does not rely on:

- implicit OpenGL fallback
- implicit CPU video upload fallback for visible playback
- Qt/QPainter whole-label rendering for transcript or speaker overlays
- diagnostic readback as a hot-path dependency

## Update Rule

Any future change to one of these items requires updating this document in the same change:

- stage list
- stage order
- shader inventory
- Vulkan pipeline object inventory
- per-clip handoff lifetime
- decoder payload contract
- REST stage-counter schema
