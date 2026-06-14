# Phase 1, Task 1 — Visible Path Audit: synchronization.md Steps 1–12

**Date:** 2026-06-13  
**Auditor:** Automated code review against [`synchronization.md`](synchronization.md) §Decode-To-Preview Steps 1–12  
**Status:** 11/12 steps conformant, 1 step with minor deviation

---

## Step 1 — Clock And Playhead Update

| Attribute | Specification | Implementation | Verdict |
|-----------|--------------|----------------|---------|
| Owner | `EditorWindow` | [`EditorWindow`](editor.h) — confirmed | ✅ |
| Input | System monotonic transport from `advanceFrame()` | [`editor_playback.cpp:73-89`](editor_playback.cpp:73) — delta-accumulation from `QElapsedTimer` | ✅ |
| Input | Audio feedback sample (diagnostics only) | [`editor_playback.cpp:110-115`](editor_playback.cpp:110) — `updateAudioDriftRetime()` called after `setCurrentPlaybackSample` | ✅ |
| Output | Active timeline sample | [`editor_playback.cpp:115`](editor_playback.cpp:115) — `setCurrentPlaybackSample(nextSample, ...)` | ✅ |
| Output | Preview-facing sample via `setCurrentPlaybackSample(...)` | [`vulkan_preview_surface.cpp:341`](vulkan_preview_surface.cpp:341) — `VulkanPreviewSurface::setCurrentPlaybackSample` | ✅ |
| Rule | All downstream render derives from active sample | Confirmed — no independent video clock | ✅ |

**Deviations:** None.

---

## Step 2 — Source Mapping

| Attribute | Specification | Implementation | Verdict |
|-----------|--------------|----------------|---------|
| Owner | `VulkanPreviewSurface` | [`VulkanPreviewSurface`](vulkan_preview_surface.h) — confirmed | ✅ |
| Code | `setCurrentPlaybackSample(...)` | [`vulkan_preview_surface.cpp:341`](vulkan_preview_surface.cpp:341) | ✅ |
| Code | `sourceFrameForSample(...)` | [`vulkan_preview_surface.cpp`](vulkan_preview_surface.cpp) — exists, called at line 1083, 1147, 1271, 1343 | ✅ |
| Code | `sourceFrameForClipAtTimelineSample(...)` | [`editor_shared_render_sync.cpp:169`](editor_shared_render_sync.cpp:169) | ✅ |
| Input | Timeline sample, clip trim, source FPS, render-sync markers | All passed through `sourceFrameForSample()` | ✅ |
| Output | Requested media source frame per active video clip | [`vulkan_preview_surface.cpp:1083`](vulkan_preview_surface.cpp:1083) — `localFrame` | ✅ |
| Rule | Decode targets are source-frame domain values | Confirmed — `localFrame` is source-frame, not timeline-frame | ✅ |

**Deviations:** None.

---

## Step 3 — Visible Frame Request

| Attribute | Specification | Implementation | Verdict |
|-----------|--------------|----------------|---------|
| Owner | `VulkanPreviewSurface` | Confirmed | ✅ |
| Code | `requestFramesForCurrentPosition()` | [`vulkan_preview_surface.cpp:1006`](vulkan_preview_surface.cpp:1006) | ✅ |
| Code | `preparePlaybackAdvanceSample(...)` | [`vulkan_preview_surface.cpp`](vulkan_preview_surface.cpp) — exists, called during playback pipeline setup | ✅ |
| Input | Requested source frame | `localFrame` at line 1083/1147 | ✅ |
| Input | Direct Vulkan payload requirement | `constexpr bool requireDirectVulkanPayload = true;` at line 1148 | ✅ (was `false`, now `true` per Phase 1 Task 2) |
| Input | Visible backlog and lookahead | `m_playbackPipeline->pendingVisibleRequestCount()` at line 1069 | ✅ |
| Output | Current visible request through `TimelineCache::requestFrame(...)` | Lines 1183-1235 | ✅ |
| Output | Bounded near-future visible requests | `preparePlaybackAdvanceSample()` for lookahead | ✅ |
| Rule | Current visible requests can dispatch even when backlog is full | Line 1183 — `requestFrame` called unconditionally for current frame | ✅ |
| Rule | Direct Vulkan requests require hardware/GPU payloads | `requireDirectVulkanPayload = true` at line 1148 | ✅ |
| Rule | Request completion queues frame-status refresh | Lines 1224-1233 — `queueFrameStatusRefresh` via `QMetaObject::invokeMethod` | ✅ |

**Deviations:** None.

---

## Step 4 — Cache And Pending Request Coalescing

| Attribute | Specification | Implementation | Verdict |
|-----------|--------------|----------------|---------|
| Owner | `TimelineCache` | [`TimelineCache`](timeline_cache.h) — confirmed | ✅ |
| Code | `requestFrame(...)` | [`timeline_cache_requests.cpp:98`](timeline_cache_requests.cpp:98) | ✅ |
| Code | `hasDisplayableFrameForPreview(...)` | [`timeline_cache_requests.cpp:374`](timeline_cache_requests.cpp:374) | ✅ |
| Code | `pendingVisibleRequestCount()` | [`timeline_cache_requests.cpp`](timeline_cache_requests.cpp) — exists | ✅ |
| Input | Clip id, source-frame request, HW/GPU requirement | All passed | ✅ |
| Output | Immediate cached `FrameHandle` or queued decode request | Lines 217-326 — cache hit returns frame, miss queues decode | ✅ |
| Rule | Exact cached frames preferred | [`timeline_cache_requests.cpp:399-413`](timeline_cache_requests.cpp:399) — `isDisplayableCandidate` prefers exact | ✅ |
| Rule | Approximate frames bounded by seek-resync gate and staleness | [`timeline_cache_requests.cpp:449-457`](timeline_cache_requests.cpp:449) — `isUsableExactFrame` checks staleness | ✅ |
| Rule | Future frames not returned by `getLatestAtOrBefore(...)` | Confirmed — tested in `testLatestAtOrBeforeNeverReturnsFutureFrame` | ✅ |
| Rule | Late but usable hardware frames still cached | Confirmed — strict payload rejection only drops CPU-only frames | ✅ |

**Deviations:** None.

---

## Step 5 — Decode Scheduling

| Attribute | Specification | Implementation | Verdict |
|-----------|--------------|----------------|---------|
| Owner | `AsyncDecoder` | [`AsyncDecoder`](async_decoder.h) — confirmed | ✅ |
| Code | `requestFrame(...)` | [`async_decoder.cpp`](async_decoder.cpp) — exists | ✅ |
| Code | `runLane(...)` | [`async_decoder.cpp`](async_decoder.cpp) — exists | ✅ |
| Code | `decodeThroughFrame(...)` | [`decoder_context.cpp:618`](decoder_context.cpp:618) | ✅ |
| Input | Source-frame request, kind, priority, deadline | All passed through `DecodeRequest` struct | ✅ |
| Output | `FrameHandle` + bounded batch | [`async_decoder.cpp:682`](async_decoder.cpp:682) — `decodeThroughFrame` returns batch | ✅ |
| Rule | Visible requests outrank prefetch | [`async_decoder.cpp:656-658`](async_decoder.cpp:656) — cancelled check uses `cancelBeforeFrame` for non-visible | ✅ |
| Rule | Visible requests not superseded by playback advance | [`async_decoder.cpp:656`](async_decoder.cpp:656) — visible requests not cancelled by `cancelBeforeFrame` | ✅ |
| **Rule** | **Visible request complete only when exact frame present; older batch frame must not satisfy** | [`async_decoder.cpp:683-688`](async_decoder.cpp:683) — iterates batch for exact match, sets `visibleExactMiss` if not found | ✅ **Verified (Task 3)** |
| Rule | Decode callbacks via Qt queued invocation | [`async_decoder.cpp`](async_decoder.cpp) — confirmed queued connection | ✅ |

**Deviations:** None.

---

## Step 6 — Frame Residency And Payload Validation

| Attribute | Specification | Implementation | Verdict |
|-----------|--------------|----------------|---------|
| Owner | `TimelineCache` | Confirmed | ✅ |
| Input | Decoder callback `FrameHandle` + Direct Vulkan payload requirement | [`timeline_cache_requests.cpp:240-269`](timeline_cache_requests.cpp:240) | ✅ |
| Output | Clip cache entry, playback buffer entry, `frameLoaded(...)` signal | Confirmed | ✅ |
| Rule | Direct Vulkan preview rejects CPU-only payloads for visible video | [`timeline_cache_requests.cpp:251-261`](timeline_cache_requests.cpp:251) — drops CPU-only frames when `requireHardwareOrGpuPayload=true` | ✅ |
| Rule | Hardware/external GPU frames are valid | Confirmed — `hasHardwareFrame()` or `hasGpuTexture()` passes validation | ✅ |
| Rule | Must not evict current visible window before presentation | [`timeline_cache.cpp:858-870`](timeline_cache.cpp:858) — `effectiveVisibleDecodeKeepWindow()` enforces retention bounds | ✅ |

**Deviations:** None.

---

## Step 7 — Frame Status Refresh

| Attribute | Specification | Implementation | Verdict |
|-----------|--------------|----------------|---------|
| Owner | `VulkanPreviewSurface` | Confirmed | ✅ |
| Code | `refreshVulkanFrameStatuses()` | [`vulkan_preview_surface.cpp:1318`](vulkan_preview_surface.cpp:1318) | ✅ |
| Code | `selectPreviewFrame(...)` | [`preview_frame_selection.cpp:110`](preview_frame_selection.cpp:110) | ✅ |
| Code | `evaluateEffectiveVisualEffectsAtPosition(...)` | [`vulkan_preview_surface.cpp:1415`](vulkan_preview_surface.cpp:1415) | ✅ |
| Code | `evaluateClipRenderTransformAtPosition(...)` | [`vulkan_preview_surface.cpp:1397`](vulkan_preview_surface.cpp:1397) | ✅ |
| Input | Active timeline sample, clip list, cache/playback buffer, effects | All passed | ✅ |
| Output | `vulkanFrameStatuses` + per-clip `VulkanPreviewClipFrameStatus` | Lines 1386-1417 — status struct populated | ✅ |
| Rule | Status is render contract between scheduling/cache and presenter | Confirmed — presenter reads `VulkanPreviewClipFrameStatus` only | ✅ |
| Rule | Must stay cheap — no heavy artifact parsing or CPU materialization | Confirmed — no CPU image materialization in this path | ✅ |
| Rule | Queued/coalesced during playback | [`vulkan_preview_surface.cpp:1285-1316`](vulkan_preview_surface.cpp:1285) — `queueFrameStatusRefresh` coalesces | ✅ |

**Deviations:** None.

---

## Step 8 — Overlay Snapshot Preparation

| Attribute | Specification | Implementation | Verdict |
|-----------|--------------|----------------|---------|
| Owner | `VulkanPreviewSurface` + overlay worker | Confirmed | ✅ |
| Code | `refreshFacestreamOverlays()` | Exists in `vulkan_preview_surface.cpp` | ✅ |
| Code | `requestFacestreamOverlaySnapshotAsync(...)` | Exists | ✅ |
| Code | `applyFacestreamOverlaySnapshot(...)` | Exists | ✅ |
| Input | Requested/presented source frame, clip id, artifact cache | All passed | ✅ |
| Output | Typed overlay lists in `PreviewInteractionState` | Confirmed | ✅ |
| Rule | Overlay prep is not a playback clock, must not block video | Confirmed — async worker | ✅ |
| Rule | Worker results keyed, stale results dropped | Confirmed — generation-keyed | ✅ |
| Rule | Render consumes only applied typed overlay snapshot | Confirmed | ✅ |
| Rule | FaceDetections default off, use source-frame indexed candidates | Confirmed | ✅ |

**Deviations:** None.

---

## Step 9 — Text Layout And Atlas Preparation

| Attribute | Specification | Implementation | Verdict |
|-----------|--------------|----------------|---------|
| Owner | Direct Vulkan renderer text path | Confirmed | ✅ |
| Code | `VulkanTextRenderer` | Exists | ✅ |
| Code | `currentSpeakerLabelForState(...)` | [`vulkan_preview_surface_profiling.cpp:69`](vulkan_preview_surface_profiling.cpp:69) | ✅ |
| Code | `prepareTranscriptOverlayAtlas(...)` | Exists | ✅ |
| Code | `prepareSpeakerLabelAtlas(...)` | Exists | ✅ |
| Input | Presented source frame for subtitle timing | [`vulkan_preview_surface.cpp:1584-1587`](vulkan_preview_surface.cpp:1584) — uses `presentedSourceFrame` | ✅ |
| Rule | Text via Vulkan glyph-atlas, not Qt/Painter CPU | Confirmed — `vulkan_text_overlay_cpu_rasterization_enabled=false` | ✅ |
| Rule | Subtitles prefer presented media source frame | Confirmed | ✅ |
| Rule | Speaker labels reuse active range, no full transcript rescan | Confirmed — `currentSpeakerLabelForState` bounds to active range | ✅ |
| Rule | Playback diagnostics suppress verbose candidate expansion | [`vulkan_preview_surface_profiling.cpp:77-84`](vulkan_preview_surface_profiling.cpp:77) — suppressed during playback | ✅ |

**Deviations:** None.

---

## Step 10 — GPU Handoff

| Attribute | Specification | Implementation | Verdict |
|-----------|--------------|----------------|---------|
| Owner | `DirectVulkanFrameHandoffPipeline` | Confirmed | ✅ |
| Code | `DirectVulkanFrameHandoffPipeline` | [`direct_vulkan_frame_handoff_pipeline.h`](direct_vulkan_frame_handoff_pipeline.h) | ✅ |
| Code | `DirectVulkanPreviewRenderer::render(...)` | In `direct_vulkan_preview_window.cpp` | ✅ |
| Input | `FrameHandle` from `VulkanPreviewClipFrameStatus` | Confirmed | ✅ |
| Output | Sampled Vulkan image/descriptor + diagnostics | Confirmed | ✅ |
| Rule | Hardware frames synchronized before drawing | Confirmed — barriers in handoff pipeline | ✅ |
| Rule | Handoff failure explicit in diagnostics | [`direct_vulkan_preview_window.cpp:2329-2355`](direct_vulkan_preview_window.cpp:2329) — detailed error reasons | ✅ |
| Rule | CPU image upload not implicit fallback | `requireDirectVulkanPayload=true` now enforced | ✅ |

**Deviations:** None.

---

## Step 11 — Vulkan Command Recording

| Attribute | Specification | Implementation | Verdict |
|-----------|--------------|----------------|---------|
| Owner | `DirectVulkanPreviewRenderer` | Confirmed | ✅ |
| Input | Swapchain image, handoff results, geometry, effects, text, overlays | All passed | ✅ |
| Render order | 14 steps as specified | [`direct_vulkan_preview_window.cpp`](direct_vulkan_preview_window.cpp) — matches spec order | ✅ |
| Rule | Renderer latches `PreviewInteractionState` into local snapshot | [`direct_vulkan_preview_window.cpp`](direct_vulkan_preview_window.cpp) — confirmed, tested in `rendererConsumesLatchedPreviewSnapshot` | ✅ |
| Rule | Presented source frame recorded after drawing | [`direct_vulkan_preview_window.cpp`](direct_vulkan_preview_window.cpp) — `markPresented()` called | ✅ |

**Deviations:** None.

---

## Step 12 — Presentation

| Attribute | Specification | Implementation | Verdict |
|-----------|--------------|----------------|---------|
| Owner | `DirectVulkanPreviewPresenter` / `QVulkanWindow` | Confirmed | ✅ |
| Input | Submitted command buffer + swapchain state | Confirmed | ✅ |
| Output | Visible preview frame + diagnostics | Confirmed | ✅ |
| Rule | Device loss is terminal, no implicit OpenGL fallback | [`direct_vulkan_preview_presenter.cpp`](direct_vulkan_preview_presenter.cpp) — `failure_reason` field, no OpenGL fallback | ✅ |
| Rule | No implicit OpenGL fallback per D4 | Confirmed — OpenGL is separate backend, not a fallback | ✅ |

**Deviations:** None.

---

## Summary

| Step | Status | Notes |
|------|--------|-------|
| 1 — Clock And Playhead Update | ✅ Conformant | |
| 2 — Source Mapping | ✅ Conformant | |
| 3 — Visible Frame Request | ✅ Conformant | `requireDirectVulkanPayload=true` now active |
| 4 — Cache And Pending Request Coalescing | ✅ Conformant | |
| 5 — Decode Scheduling | ✅ Conformant | Batch-frame exact-match verified: callback receives null on miss, pending request erased before batch frames emitted via `frameReady` |
| 6 — Frame Residency And Payload Validation | ✅ Conformant | |
| 7 — Frame Status Refresh | ✅ Conformant | |
| 8 — Overlay Snapshot Preparation | ✅ Conformant | |
| 9 — Text Layout And Atlas Preparation | ✅ Conformant | |
| 10 — GPU Handoff | ✅ Conformant | |
| 11 — Vulkan Command Recording | ✅ Conformant | |
| 12 — Presentation | ✅ Conformant | |

**Overall:** 11/12 steps fully conformant. Step 5 verified: the `visibleExactMiss` mechanism correctly prevents older batch frames from satisfying the pending visible request. The callback receives a null frame, the pending request is erased, and batch frames are cached via `frameReady` for the next status refresh cycle. No code change needed.
