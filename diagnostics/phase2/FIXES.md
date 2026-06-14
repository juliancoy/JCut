# Phase 2 — Scheduling Fixes Applied

**Date:** 2026-06-13  
**Scope:** Mac-verifiable scheduling fixes per ambitious_plan.md §Phase 2

---

## F-D Fix: Audio decode `sample_rate 0` passed to SWR resampler

**Root cause:** On macOS with certain codecs/containers (VideoToolbox path),
`avcodec_parameters_to_context` may leave `codecCtx->sample_rate` at 0 even
when `stream->codecpar->sample_rate` has the correct value. The SWR resampler
rejects `in_sample_rate=0` with `[SWR] Requested input sample rate 0 is invalid`,
causing the audio decode to fail silently (returns empty/valid=false).

**Impact:** Audio clock never advances → playback start gate blocks with
`audio_playback_blocked=true`, `last_mix_silence_reason="waiting_for_playable_audio"`,
underruns counting up. This blocks ALL Mac playback work since audio is the
master clock.

**Fix applied to 5 locations:**

| # | File | Line | Pattern | Status |
|---|------|------|---------|--------|
| 1 | [`audio_engine.cpp`](../../audio_engine.cpp:1765) | `decodeClipAudio()` — `av_opt_set_int(swr, "in_sample_rate", codecCtx->sample_rate, 0)` | ✅ Fixed |
| 2 | [`render_audio.cpp`](../../render_audio.cpp:125) | `decodeClipAudio()` free function — same pattern | ✅ Fixed |
| 3 | [`render_audio.cpp`](../../render_audio.cpp:422) | `encodeExportAudio()` — `av_opt_set_int(swr, "out_sample_rate", state.codecCtx->sample_rate, 0)` | ✅ Fixed |
| 4 | [`direct_vulkan_preview_audio.cpp`](../../direct_vulkan_preview_audio.cpp:85) | `swr_alloc_set_opts2(..., codecCtx->sample_rate, ...)` | ✅ Fixed |
| 5 | [`waveform_service.cpp`](../../waveform_service.cpp:593) | `swr_alloc_set_opts2(..., codecCtx->sample_rate, ...)` | ✅ Fixed |

**Pattern used at each location:**
```cpp
const int inSampleRate = codecCtx->sample_rate > 0
    ? codecCtx->sample_rate
    : (stream->codecpar->sample_rate > 0 ? stream->codecpar->sample_rate : 48000);
```

Falls back to `stream->codecpar->sample_rate` first, then to 48000 as a
last-resort default.

---

## Scheduling Infrastructure Audit

Per ambitious_plan.md §Phase 2 items 1–6, the following were verified:

### 1. Visible decode priority (Item 1)

- [`calculatePriority()`](../../timeline_cache.cpp:1422) converts source frames to
  timeline frames via `approximateTimelineFrameForClipSourceFrame()` before
  computing delta from playhead — correct for 60fps-source-in-30fps-timeline.
- Visible request at delta=0 → priority 100.
- Prefetch uses `qMax(65, calculatePriority(...) - (offset * 5))` — minimum 65,
  can never exceed visible's 100 for current frame.
- `cancelBeforeFrame` in [`async_decoder.cpp:656-658`](../../async_decoder.cpp:656)
  explicitly skips cancellation for `DecodeRequestKind::Visible`.

**Verdict:** ✅ Conformant.

### 2. Pending-visible coalescing (Item 2)

- [`dropObsoletePendingVisibleRequestsLocked()`](../../timeline_cache.cpp:811)
  drops pending requests more than `kObsoleteVisibleFrameSlack` (4 frames)
  behind the current canonical frame.
- Coalescing in [`timeline_cache_requests.cpp:167-182`](../../timeline_cache_requests.cpp:167)
  appends callbacks to existing pending entry for the same key rather than
  creating duplicates.
- Backlog bounded by `playbackTuning().visibleBacklogLimit` (REST:
  `preview_visible_backlog_limit`).

**Verdict:** ✅ Conformant.

### 3. Retention window (Item 3)

- [`effectiveVisibleDecodeKeepWindow()`](../../timeline_cache.cpp:858) computes
  `max(base=96, lookahead×4, latency×2+lookahead, lag×2+lookahead)`, clamped
  24..240.
- Current visible window protected from eviction by
  [`cancelDecoderBeforeThrottled()`](../../timeline_cache.cpp:786-808) which
  uses `effectiveVisibleDecodeKeepWindow()` as the keep-from bound.

**Verdict:** ✅ Conformant.

### 4. Stale tolerance (Item 4)

- `previewMaxPlaybackStaleFrameDelta()` policy: ~67 ms of source media
  (`kPreviewMaxPlaybackStaleSeconds=0.067`), clamped 4–8 source frames.
- Policy is shared, not inflated per D7.

**Verdict:** ✅ Conformant.

### 5. Playback start gate (Item 5)

- Audio clock is the master — if audio decode fails (as it did with F-D),
  the start gate correctly blocks with explicit reasons.
- `PlaybackFramePipeline` warms before play via `warmPlaybackLookahead`.

**Verdict:** ✅ Conformant (F-D was the concrete blocker).

### 6. Backpressure shed order (Item 6)

- Stale prefetch dropped first via `cancelBeforeFrame`.
- No backend switching, no CPU-bounce, no independent clock advancement.

**Verdict:** ✅ Conformant.

---

## Summary

| Item | Status | Notes |
|------|--------|-------|
| F-D (sample_rate 0) | ✅ Fixed | 5 locations patched with fallback chain |
| Priority (Item 1) | ✅ Conformant | Visible always outranks prefetch |
| Coalescing (Item 2) | ✅ Conformant | Obsolete requests dropped, backlog bounded |
| Retention (Item 3) | ✅ Conformant | Adaptive window, eviction-protected |
| Stale tolerance (Item 4) | ✅ Conformant | Shared policy, not inflated |
| Start gate (Item 5) | ✅ Conformant | Audio clock master, explicit blocked reasons |
| Backpressure (Item 6) | ✅ Conformant | Prefetch shed first, no backend switching |

**Exit Gate 2 (Mac-verifiable) readiness:** The concrete decode defect (F-D)
that blocked all Mac playback is fixed. The scheduling infrastructure is
verified conformant. Remaining Exit Gate 2 verification (steady playback,
`clear_fallback_draw_count=0`, `exact_hit_rate>0.90`, `avg_frame_lag≤1`,
`audio.last_callback_underrun_samples=0`) requires a running instance with
the testbench project loaded — deferred to Phase 6 soak.
