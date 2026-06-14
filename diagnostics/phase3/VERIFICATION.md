# Phase 3 — Stream Alignment Verification

**Date:** 2026-06-13  
**Scope:** Verify single-clock architecture, A/V lock, overlay buffering, bounded reads, export alignment

---

## 1. Single-Clock Proof (TIME.md Invariants 1–10)

| Element | Clock Source | Converter | Verdict |
|---------|-------------|-----------|---------|
| Video frame | System-clock transport via `advanceFrame()` delta-accumulation | `sourceFrameForClipAtTimelineSample()` in [`editor_shared_render_sync.cpp:169`](../../editor_shared_render_sync.cpp:169) | ✅ |
| Audio sample | System-clock transport via `advanceFrame()` delta-accumulation | `sourceSampleForClipAtTimelineSample()` in [`editor_shared_render_sync.cpp:201`](../../editor_shared_render_sync.cpp:201) | ✅ |
| Transcript frame | System-clock transport via `advanceFrame()` | `transcriptFrameForClipAtTimelineSample()` in [`editor_shared_render_sync.cpp:222`](../../editor_shared_render_sync.cpp:222) | ✅ |
| Speaker-label timing | System-clock transport via `advanceFrame()` | `currentSpeakerLabelForState()` in [`vulkan_preview_surface_profiling.cpp:69`](../../vulkan_preview_surface_profiling.cpp:69) | ✅ |
| Overlay timing | Presented source frame from `VulkanPreviewClipFrameStatus` | [`vulkan_preview_surface.cpp:1584-1587`](../../vulkan_preview_surface.cpp:1584) — prefers `presentedSourceFrame` | ✅ |
| FaceDetections | Source-frame-indexed candidates | [`facedetections_time_mapping.h`](../../facedetections_time_mapping.h) — `facedetectionsLookupFrameForDomain()` | ✅ |

**Subtitles prefer presented media source frame:** Confirmed at [`vulkan_preview_surface.cpp:1584-1587`](../../vulkan_preview_surface.cpp:1584):
```cpp
const qint64 subtitleSourceFrame =
    status.hasFrame && status.presentedSourceFrame >= 0
        ? status.presentedSourceFrame
        : status.requestedSourceFrame;
```
Text never leads late video.

---

## 2. A/V Lock

- System-clock transport is the playback authority ([`editor_playback.cpp:73-89`](../../editor_playback.cpp:73)).
- Audio feedback is the *effective audible* position, not queued/submitted position
  ([`playback_clock_coordinator.cpp:16-27`](../../playback_clock_coordinator.cpp:16)).
- In pitch-preserving `time_stretch`, video holds when retimed segment is blocked:
  [`audio_engine.cpp:1273-1288`](../../audio_engine.cpp:1273) — `driftRetimeRate` bounds the
  correction; [`editor_playback.cpp:110-115`](../../editor_playback.cpp:110) —
  `updateAudioDriftRetime()` called after `setCurrentPlaybackSample`, so audio feedback
  modulates but does not override the transport clock.

**Verdict:** ✅ Conformant.

---

## 3. Caption/Overlay Buffering

- Overlay workers use async snapshot pattern with generation keys:
  - `requestFacestreamOverlaySnapshotAsync()` — async worker
  - `applyFacestreamOverlaySnapshot()` — applies result with generation check
- Stale worker results dropped by key/generation mismatch.
- Prior snapshot held briefly (no blocking on worker).
- FaceDetections default **off** (`facedetectionsOverlays` empty unless explicitly enabled).
- Source-frame-indexed candidates via `facedetectionsLookupFrameForDomain()`.

**Verdict:** ✅ Conformant.

---

## 4. Bounded Continuity Reads

- Preview/Speakers overlays use bounded current-playhead reader over `tracks.idx`.
- `continuityStreamsForRoot()` is called from:
  - `speakers_tab.cpp` — UI interaction paths (clip selection, speaker details), not hot preview
  - `tracks.cpp:533` — `rawFallbackAllowed` path only (stored streams empty AND no raw artifact)
  - `editor_facedetections_preview_harness.cpp` — test harness
- The hot preview path (`vulkan_preview_surface.cpp`) does NOT call `continuityStreamsForRoot()`.
  It uses the typed overlay lists in `PreviewInteractionState` prepared by the async overlay worker.

**Verdict:** ✅ Conformant. No UI path calls `continuityStreamsForRoot` on every tick.

---

## 5. Export-Path Alignment

- Export uses separate decoder/cache from preview:
  - [`render_audio.cpp:54`](../../render_audio.cpp:54) — `decodeClipAudio()` free function
  - [`render_export.cpp`](../../render_export.cpp) — standalone export renderer
- 60fps export of 30fps edit range: each output-PTS maps to fractional timeline position
  via `sourceFrameForClipAtTimelinePosition()` in [`editor_shared_render_sync.cpp:119`](../../editor_shared_render_sync.cpp:119).
- `scripts/vulkan_headless_export_compare.sh` exists for preview/export comparison.

**Verdict:** ✅ Conformant.

---

## Summary

| Task | Status | Notes |
|------|--------|-------|
| 1. Single-clock proof | ✅ Conformant | All elements derive from system-clock transport |
| 2. A/V lock | ✅ Conformant | Transport is authority, audio is feedback |
| 3. Caption/overlay buffering | ✅ Conformant | Async workers, generation-keyed, non-blocking |
| 4. Bounded continuity reads | ✅ Conformant | No `continuityStreamsForRoot` in hot path |
| 5. Export-path alignment | ✅ Conformant | Separate decoder/cache, fractional PTS mapping |

**Exit Gate 3 readiness:** All structural requirements verified. Test execution
(`test_temporal_sync_contract`, `test_transcript_logic`, etc.) requires a running
build — deferred to Phase 6 soak.
