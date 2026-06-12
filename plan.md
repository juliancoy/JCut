# Overlay GPU Implementation Plan

> **Doc status (2026-06-11):** Overlay-GPU plan; largely implemented. One acceptance criterion
> conflicts with the present code: this plan requires vulkan_visible_cpu_upload_fallback_enabled
> = false, but vulkan_preview_surface_profiling.cpp:87,292 hardcode it true and the cpu_upload
> visible decode path is reachable (vulkan_preview_surface.cpp:1469). Whether strictness was
> deliberately relaxed or regressed is under investigation (ambitious_plan.md Phase 0, decision
> D6). Until resolved, treat the strict criterion as the target, not the present state.

## Current Finding

The visible video frame path is already enforcing the correct direct Vulkan contract:

- Visible decode requests require a direct Vulkan-capable payload.
- CPU image upload fallback is disabled for the direct Vulkan preview path.
- The live pipeline reports `decode_path=hardware_frame`, `cpu_image=false`, `last_handoff_mode=hardware_direct`, and zero handoff failures.
- The presenter draws the active clip through the Vulkan texture path, not through `QImage`.

The remaining non-final part was overlay preparation. Face/speaker overlay metadata was resolved on the preview/UI refresh path before the GPU draw. The GPU draws the resulting overlay primitives, but the lookup, interpolation, filtering, and debug assembly are CPU-side work.

Current implementation status:

- During playback, cached FaceDetections overlay preparation now runs through a worker-backed typed snapshot request.
- The worker payload builder is extracted into `facestream_overlay_snapshot.{h,cpp}` with a pure request/result boundary and focused unit coverage.
- The UI thread applies the latest completed snapshot and continues to own presentation state and hit testing.
- Paused/seeked overlay refresh may still hydrate FaceDetections artifacts synchronously. This is intentional for the current KISS boundary because file/artifact ownership has not been moved into the worker.
- The direct Vulkan video path remains unchanged and still rejects CPU image upload as a normal preview path.
- Transcript subtitles and current-speaker labels use the direct Vulkan text renderer. CPU font/layout preparation is explicit and test-covered; draw submission is the Vulkan glyph-atlas pass, not ImGui, Qt painter, or whole-label CPU image upload.

## Problem

`VulkanPreviewSurface::refreshVulkanFrameStatuses()` calls `refreshFacestreamOverlays()` during preview refresh. In playback, that function now queues cached overlay work to a worker and returns without doing track interpolation on the UI thread. Outside playback, it can still hydrate the FaceDetections cache synchronously.

The remaining accepted boundaries are:

- First use after a cache miss still needs a non-playback hydration point.
- Overlay snapshots may lag the playhead by one worker completion under high churn; the UI keeps the prior snapshot and immediately requeues for the active sample rather than blocking playback.
- Full GPU-buffer batching for overlay geometry remains a renderer optimization, not a correctness dependency for the direct Vulkan playback contract.

## Target Architecture

### Temporal Source Of Truth

The active playhead sample remains the single source of truth.

The overlay pipeline must derive all frame-local state from the same canonical timeline sample/source-frame conversion used by video, captions, speaker labels, and transcript overlays. Overlay workers must not invent a second clock.

### UI Thread

The UI thread should only:

- Advance/apply the current playhead sample.
- Select the best already-prepared overlay snapshot for that sample.
- Publish immutable overlay state to the presenter.
- Handle hit testing against the current overlay snapshot.
- Queue worker requests when the current/near-future snapshot is missing or stale.

The UI thread should not:

- Load FaceDetections artifacts.
- Parse JSON/CBOR/IDX artifacts.
- Rebuild continuity streams.
- Iterate all track history for every visible playback frame.
- Perform expensive debug object construction in the playback hot path.

### Worker Thread

The overlay worker should:

- Receive immutable snapshot requests keyed by clip id, source frame, timeline sample, overlay source filter, visibility flags, artifact revision, and render-sync revision.
- Resolve face/speaker overlay metadata for current and near-future frames.
- Return compact frame-local overlay snapshots.
- Preserve deterministic ordering for hit testing and rendering.
- Avoid touching UI-owned state directly.

The worker output should be a typed structure, not JSON. JSON is acceptable only at REST/debug serialization boundaries.

### GPU Presenter

The direct Vulkan presenter should:

- Continue drawing video from hardware/external Vulkan payloads only.
- Draw face/speaker/caption overlay primitives from prepared typed overlay snapshots.
- Use GPU buffers/descriptors for overlay geometry where the data is stable enough to batch.
- Avoid readback or `QImage` materialization for normal preview presentation.

Overlay text uses a dedicated Vulkan glyph-atlas renderer. CPU work is limited to font resolution, glyph atlas construction, and layout generation; draw submission is through Vulkan resources and shaders. ImGui remains scoped to the standalone FaceDetections preview tool and is not a subtitle/speaker-label backend for editor playback.

## Implementation Steps

1. Define an immutable overlay snapshot model.
   - Status: implemented for Vulkan FaceDetections playback overlay snapshots.
   - Frame-local face boxes and raw detections use a typed request/result model in `facestream_overlay_snapshot`.
   - Speaker labels and transcript captions use typed semantic state from `PreviewInteractionState`/`TranscriptOverlayLayout` and are rendered by `VulkanTextRenderer`.
   - Hit testing continues to use the current applied typed overlay snapshot on the UI thread.

2. Extract overlay preparation into a worker-owned service.
   - Status: implemented for cached playback overlay preparation.
   - Heavy snapshot construction is isolated behind a pure request/result boundary in `facestream_overlay_snapshot`.
   - Inputs are copied or shared as immutable cached runtime data.
   - Results are delivered back to the UI thread through a queued callback.

3. Keep artifact/runtime caches outside the frame hot path.
   - Status: implemented for playback cache misses by preserving previous overlays instead of loading artifacts.
   - FaceDetections artifacts should be loaded once per artifact revision.
   - Playback refresh should do numeric lookup/interpolation only.
   - No JSON parsing or artifact discovery should appear under active playback call stacks.

4. Add near-future overlay prefetch.
   - Status: implemented as bounded newest-request prefetch/coalescing.
   - Current worker path resolves the active requested snapshot and keeps one newest queued follow-up request while work is in flight.
   - Queue depth remains bounded at one in-flight worker request plus one newest queued request.
   - Stale requests are dropped by request key/request id when the playhead jumps, and the queued follow-up is launched immediately after the in-flight worker completes.

5. Apply snapshots on the UI thread.
   - Status: implemented.
   - The UI thread atomically swaps the current overlay snapshot when the result still matches the active generation.
   - If a result is late, keep the prior snapshot briefly rather than blocking playback.
   - Hit testing always uses the currently applied snapshot.

6. Move Vulkan overlay draw inputs to GPU-friendly buffers.
   - Status: current renderer draws overlay primitives from prepared typed state; explicit GPU-buffer batching remains an optimization.
   - Batch face boxes and raw detection boxes into dynamic vertex/uniform data.
   - Upload only changed frame-local overlay data.
   - Keep normal video presentation independent from overlay preparation latency.

7. Make REST/perf diagnostics explicit.
   - Status: implemented for current worker state.
   - Report overlay preparation thread, pending key, queued key, queued clip count, last prep ms, last apply latency ms, snapshot age ms, stale/drop counts, and cache hit/miss counts.
   - Keep separate fields for video zero-copy status and overlay preparation status.

8. Add regression tests.
   - Status: implemented for the extracted overlay snapshot builder, direct Vulkan text generation, and direct Vulkan path contract.
   - `test_facestream_overlay_snapshot` covers deterministic overlay ordering, assignment-interaction overlay generation, source filtering, and raw-detection bridge/edge-hold behavior.
   - `test_vulkan_text_generation` covers speaker-label and transcript glyph atlas/layout generation plus atlas-key invalidation.
   - `test_direct_vulkan_handoff_pipeline_contract` asserts the direct Vulkan path exposes the no-CPU-upload/no-Qt-text fallback contract and that the overlay worker retains newest coalesced requests.

## Acceptance Criteria

- During playback, `perf` does not show artifact loading under `VulkanPreviewSurface::refreshFacestreamOverlays()`.
- REST reports `vulkan_visible_decode_requires_direct_vulkan_payload=true`.
- REST reports `vulkan_visible_cpu_upload_fallback_enabled=false`.
- REST reports overlay preparation as worker-backed during playback, with queue and timing fields.
- `/pipeline` continues to report hardware/external Vulkan handoff for visible video frames.
- Face/speaker overlays remain visible and clickable during playback.
- Track selection and hover behavior use the current applied overlay snapshot regardless of active tab.
- Captions, speaker labels, and face/speaker overlays derive from the same active playhead sample conversion path.
- No implicit OpenGL or CPU upload fallback is introduced.

## Non-Goals

- Do not replace the direct Vulkan video handoff path.
- Do not reintroduce `QImage` as a normal Vulkan preview presentation path.
- Do not create a second overlay timing system.
- Do not block playback waiting for non-critical overlay metadata.
- Do not move project state ownership into the overlay worker.

## Current Code Touch Points

- `vulkan_preview_surface.cpp`
  - `refreshVulkanFrameStatuses()`
  - direct payload readiness checks
  - playback lookahead readiness

- `vulkan_preview_surface_facedetections.cpp`
  - `refreshFacestreamOverlays()`
  - face/speaker overlay lookup
  - raw detection lookup
  - hit-test overlay state population

- `facestream_overlay_snapshot.{h,cpp}`
  - typed playback overlay snapshot request/result model
  - deterministic face/raw overlay snapshot construction
  - raw-detection bridge/edge-hold lookup used by worker and tests

- `vulkan_preview_surface_profiling.cpp`
  - REST/perf fields separating video zero-copy status from overlay preparation status

- `direct_vulkan_preview_window.cpp`
- `direct_vulkan_preview_geometry.cpp`
- `direct_vulkan_preview_interaction.cpp`
  - Vulkan clip draw
  - overlay primitive draw
  - direct-path fallback/error reporting

- `vulkan_text_renderer.{h,cpp}`
  - Vulkan glyph-atlas text generation and draw submission
  - speaker-label and transcript text layout test hooks

- `direct_vulkan_frame_handoff_pipeline.cpp`
  - direct hardware/external frame handoff
  - explicit CPU fallback rejection

## Status

Video frame delivery and cached playback overlay preparation are in the final maintainable shape for the current direct Vulkan preview contract. The overlay worker now has a bounded, typed, test-covered snapshot builder. Transcript subtitles and current-speaker labels render through the Vulkan glyph-atlas text pass, with test coverage for generation semantics.

Remaining item is explicitly optimization work rather than a correctness blocker:

- renderer-level GPU-buffer batching for face/raw overlay geometry

## Verification

Current local verification:

- `cmake --build build --target jcut -j$(nproc)` passes.
- Focused Vulkan overlay/text/synchronization regression tests pass:
  - `test_facestream_overlay_snapshot`
  - `test_direct_vulkan_handoff_pipeline_contract`
  - `test_vulkan_text_generation`
  - `test_vulkan_subtitle_render`
- Full `ctest --test-dir build --output-on-failure` passes: 39/39 tests.
