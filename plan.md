# Overlay GPU Implementation Plan

## Current Finding

The visible video frame path is already enforcing the correct direct Vulkan contract:

- Visible decode requests require a direct Vulkan-capable payload.
- CPU image upload fallback is disabled for the direct Vulkan preview path.
- The live pipeline reports `decode_path=hardware_frame`, `cpu_image=false`, `last_handoff_mode=hardware_direct`, and zero handoff failures.
- The presenter draws the active clip through the Vulkan texture path, not through `QImage`.

The remaining non-final part was overlay preparation. Face/speaker overlay metadata was resolved on the preview/UI refresh path before the GPU draw. The GPU draws the resulting overlay primitives, but the lookup, interpolation, filtering, and debug assembly are CPU-side work.

Current implementation status:

- During playback, cached FaceDetections overlay preparation now runs through a worker-backed typed snapshot request.
- The UI thread applies the latest completed snapshot and continues to own presentation state and hit testing.
- Paused/seeked overlay refresh may still hydrate FaceDetections artifacts synchronously. This is intentional for the current KISS boundary because file/artifact ownership has not been moved into the worker.
- The direct Vulkan video path remains unchanged and still rejects CPU image upload as a normal preview path.

## Problem

`VulkanPreviewSurface::refreshVulkanFrameStatuses()` calls `refreshFacestreamOverlays()` during preview refresh. In playback, that function now queues cached overlay work to a worker and returns without doing track interpolation on the UI thread. Outside playback, it can still hydrate the FaceDetections cache synchronously.

The remaining risks are:

- First use after a cache miss still needs a non-playback hydration point.
- Overlay snapshots may lag the playhead by one worker completion under high churn; the UI keeps the prior snapshot rather than blocking playback.
- Full GPU-buffer batching for overlay geometry is still a separate renderer optimization.

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

Overlay text may continue through the existing text/render resources initially, but the boundary must be explicit: CPU text/layout preparation is separate from Vulkan draw submission.

## Implementation Steps

1. Define an immutable overlay snapshot model.
   - Status: implemented for Vulkan FaceDetections playback overlay snapshots.
   - Add a small typed model for frame-local face boxes, raw detections, speaker labels, transcript/caption overlay timing references, and hit-test metadata.
   - Keep it independent from `VulkanPreviewSurface` member mutation.

2. Extract overlay preparation into a worker-owned service.
   - Status: partially implemented as a worker-backed cached playback path in `VulkanPreviewSurface`.
   - Move the heavy parts of `refreshFacestreamOverlays()` into a service with a pure request/result boundary.
   - Inputs are copied or shared as immutable cached runtime data.
   - Results are delivered back to the UI thread through a queued callback.

3. Keep artifact/runtime caches outside the frame hot path.
   - Status: implemented for playback cache misses by preserving previous overlays instead of loading artifacts.
   - FaceDetections artifacts should be loaded once per artifact revision.
   - Playback refresh should do numeric lookup/interpolation only.
   - No JSON parsing or artifact discovery should appear under active playback call stacks.

4. Add near-future overlay prefetch.
   - Status: not implemented yet. Current worker path resolves the current requested snapshot and coalesces pending requests.
   - Request overlays for the current frame and a bounded lookahead window.
   - Keep queue depth bounded.
   - Drop stale requests by generation id when the playhead jumps.

5. Apply snapshots on the UI thread.
   - Status: implemented.
   - The UI thread atomically swaps the current overlay snapshot when the result still matches the active generation.
   - If a result is late, keep the prior snapshot briefly rather than blocking playback.
   - Hit testing always uses the currently applied snapshot.

6. Move Vulkan overlay draw inputs to GPU-friendly buffers.
   - Status: existing presenter still draws overlay primitives from prepared state; explicit GPU-buffer batching remains future work.
   - Batch face boxes and raw detection boxes into dynamic vertex/uniform data.
   - Upload only changed frame-local overlay data.
   - Keep normal video presentation independent from overlay preparation latency.

7. Make REST/perf diagnostics explicit.
   - Status: implemented for current worker state.
   - Report overlay preparation thread, queue depth, last request key, last prep ms, last apply latency ms, snapshot age ms, stale/drop counts, and cache hit/miss counts.
   - Keep separate fields for video zero-copy status and overlay preparation status.

8. Add regression tests.
   - Status: focused existing playback/audio tests pass. Dedicated overlay worker tests are still pending.
   - Unit test request key invalidation.
   - Unit test deterministic overlay ordering and hit-test identity.
   - Unit test stale worker results are dropped.
   - Add a focused playback/perf smoke test that asserts the direct Vulkan path has no CPU image payloads and overlay prep does not run synchronously in the playback hot path.

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

- `vulkan_preview_surface_profiling.cpp`
  - REST/perf fields separating video zero-copy status from overlay preparation status

- `direct_vulkan_preview_backend.cpp`
  - Vulkan clip draw
  - overlay primitive draw
  - direct-path fallback/error reporting

- `direct_vulkan_frame_handoff_pipeline.cpp`
  - direct hardware/external frame handoff
  - explicit CPU fallback rejection

## Status

Video frame delivery is close to final. Playback overlay preparation now has a bounded, typed, worker-backed snapshot path for cached FaceDetections data. Remaining work is limited to dedicated overlay worker tests, optional near-future overlay prefetch, and renderer-level GPU-buffer batching for overlay geometry.

## Verification

Current local verification:

- `cmake --build build --target jcut -j2` passes.
- Focused temporal/audio regression tests pass:
  - `test_playback_policy`
  - `test_audio_time_stretch`
  - `test_audio_time_stretch_cache`
- Full `ctest --test-dir build --output-on-failure` currently has unrelated environment/data failures:
  - `test_transcript_tab_follow` did not load expected transcript table rows.
  - `test_track_avatar_geometry` could not find expected continuity tracks for the selected Baltimore County clip.
  - `test_ui_smoke_harness_offscreen`, `test_speaker_flow_e2e_offscreen`, and `test_rest_smoke_offscreen` could not start cleanly because another editor instance was already running.
  - `test_ui_responsiveness_budget_offscreen` timed out waiting for playback to start in the offscreen placeholder harness.
