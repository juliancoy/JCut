# Playback Preview Glitching — Root Cause Analysis

## The Real Problem

The previous approach (tuning constants, adding dedup/coalescing) treated symptoms, not the root cause. After deep analysis of the architecture, here are the **fundamental design problems**:

---

## Root Cause 1: Dual Independent Request Paths Competing for the Same Frames

### Architecture

```
VulkanPreviewSurface::refreshVisibleFrames()
  |
  +---> TimelineCache::requestFrame()     [cache path]
  |       |
  |       +---> AsyncDecoder::requestFrame()  [decode request]
  |
  +---> PlaybackFramePipeline::requestFramesForSample()  [playback path]
          |
          +---> AsyncDecoder::requestFrame()  [ANOTHER decode request]
```

Both paths issue **independent** decode requests to the `AsyncDecoder` for the **same clip at the same frame number**. The `AsyncDecoder` has no concept of "these two requests are for the same frame from different consumers" — it sees them as separate requests.

### What Happens During Playback

1. `VulkanPreviewSurface::refreshVisibleFrames()` is called every frame tick
2. Line 1068: `if (m_interaction.playing && m_playbackPipeline)` → takes the **playback path**, calls `m_playbackPipeline->requestFramesForSample()`
3. `requestFramesForSample()` calls `schedulePlaybackWindow()` which calls `m_decoder->requestFrame()` for each frame in the window
4. **But** the `TimelineCache` path (lines 1138-1238) is **NOT taken** during playback — the early return at line 1131 prevents it

So the dual-path issue is **not** the problem during playback. The problem is elsewhere.

---

## Root Cause 2: Frame-by-Frame Request Churn (The Real Problem)

### The Tick Loop

Every frame tick during playback:

1. `VulkanPreviewSurface::refreshVisibleFrames()` is called
2. It calls `m_playbackPipeline->requestFramesForSample(currentSample, callback)`
3. `requestFramesForSample()` iterates active clips and calls `schedulePlaybackWindow()` for each
4. `schedulePlaybackWindow()` iterates `offset = 0..playbackWindowAhead` and issues decode requests

### The Churn

The problem is that **every tick** re-evaluates the entire window. Consider:

- **Tick N**: Playhead at frame 100. Window = frames 100, 101, 102, 103. Issues requests for all 4.
- **Tick N+1**: Playhead at frame 101. Window = frames 101, 102, 103, 104. Issues requests for all 4.

Frame 101 was already requested in tick N, but by tick N+1:
- The `AsyncDecoder` may have already **superseded** the request for frame 101 (because frame 102 arrived and was within `supersede_slack_frames=4`)
- Or the request is still queued but the `PlaybackFramePipeline`'s pending set (`m_pendingVisibleRequests`) already contains it, so it skips

**But** — the `cancelDecoderBeforeThrottled()` call at line 582-583 cancels all queued requests before `keepFromFrame`:
```cpp
cancelDecoderBeforeThrottled(info.playbackPath,
    qMax<int64_t>(0, canonicalFrame - keepWindow));
```

With `keepWindow = 16` (previously 8), this cancels everything before frame 84 (100-16). This is fine for old frames, but the problem is that **new requests for the same frame keep getting issued and superseded**.

### The Real Churn Pattern

1. Tick N: Request frame 100 (visible), frames 101-103 (prefetch)
2. AsyncDecoder queues them. Frame 100 starts decoding.
3. Tick N+1: Playhead moves to 101. 
   - `cancelDecoderBeforeThrottled()` cancels everything before frame 85 (101-16). Frame 100's decode continues (it's already in progress).
   - New requests: frame 101 (visible), frames 102-104 (prefetch)
   - `collectSupersededRequests()` sees frame 101 is within `supersede_slack_frames=12` of queued frame 102 → **coalesces** (with our fix)
   - But frame 100's request was already dispatched to a worker lane. When it completes, it's still within `lateBufferSeedSlack=4` of playhead 101, so it gets buffered. **This is fine.**

4. Tick N+2: Playhead at 102.
   - Cancel before frame 86.
   - New requests: frame 102 (visible), 103-105 (prefetch)
   - Frame 101 might still be queued. If so, coalescing works.
   - **But** frame 101 might have been picked up by a worker and is now decoding. Frame 102's request is new.

The issue is that **each tick generates a new visible request for the current playhead frame**, and the previous tick's visible request may still be queued/in-flight. With coalescing, nearby requests chain callbacks. But the fundamental problem is:

**Every tick generates N new decode requests, and the decoder can only process ~1-2 frames per tick.**

---

## Root Cause 3: No Backpressure or Rate Limiting

The system has no mechanism to say "we've already requested enough frames, wait for them to complete before requesting more." The `pendingVisibleRequestCount()` check in `VulkanPreviewSurface` (line 1072) checks backlog, but:

1. It checks `m_playbackPipeline->pendingVisibleRequestCount()` which counts requests in the pipeline's pending set
2. But the pipeline's pending set only tracks requests **it** issued, not what's actually in the decoder queue
3. The `AsyncDecoder` has its own queue with `kMaxPendingRequests = 128` per lane

So the pipeline can keep issuing requests even when the decoder is saturated.

---

## Root Cause 4: Cancel-Before Throttle Is Too Aggressive

`cancelDecoderBeforeThrottled()` cancels all queued requests for a file before `keepFromFrame`. With `keepWindow = 16`:
- At playhead 100: cancel before frame 84
- At playhead 101: cancel before frame 85

This means any prefetch request for frames 85-99 that was issued in a previous tick but hasn't been picked up by a worker yet gets cancelled. Then in the next tick, a new request for the same frame might be issued (if it falls within the new window).

**The cancel-before is necessary to prevent the decoder from wasting time on frames behind the playhead, but it's too aggressive for prefetched frames that are still ahead of the playhead.**

---

## Root Cause 5: No Frame Reuse Across Ticks

The `PlaybackFramePipeline` has a `PlaybackBuffer` that stores completed frames. But the **request dispatch** in `schedulePlaybackWindow()` doesn't check if a frame was already requested in a previous tick and is still in-flight. It only checks:
1. `isFrameBuffered()` — already completed
2. `m_pendingVisibleRequests.contains(key)` — already pending

But between these two states, there's a gap: the request was dispatched to the decoder, the decoder picked it up, but it hasn't completed yet. In that case, the request is **not** in `m_pendingVisibleRequests` (it was removed when picked up by the worker lane), and it's **not** in the buffer (not completed yet).

Wait — actually, `m_pendingVisibleRequests` is only cleared in the **callback** (line 708), not when the decoder picks up the request. So the request remains in `m_pendingVisibleRequests` until the callback fires. This means the pipeline **does** track in-flight requests correctly.

---

## Corrected Root Cause Analysis

After deeper analysis, the actual problems are:

### Problem A: Superseding Kills In-Flight Work

The `AsyncDecoder`'s `collectSupersededRequests()` removes queued requests for the same file when a new request arrives for a frame that's more than `supersede_slack_frames` away. With the old value of 4, this was extremely aggressive. With the new value of 12, it's better, but:

**The fundamental issue**: When the playhead moves by 1 frame per tick, and the window is 4 frames ahead, each tick generates 5 new requests. The decoder processes ~1-2 frames per tick. So requests pile up. When they pile up past `supersede_slack_frames`, they get superseded and return null callbacks.

**The coalescing fix helps**: Nearby requests chain callbacks instead of superseding. But this only works for requests within `supersede_slack_frames` of each other.

### Problem B: No Priority-Based Queue Management

All visible requests have priority 100. All prefetch requests have priority `max(80, 99 - offset)`. So:
- Visible: 100
- Prefetch offset 0: 99
- Prefetch offset 1: 98
- Prefetch offset 2: 97
- Prefetch offset 3: 96

The decoder processes in priority order. But when the queue fills up (128 per lane), it drops the lowest-priority request. This means **prefetch requests for frames further ahead get dropped first**, which is correct behavior.

### Problem C: The Real Issue — Request Volume

The real problem is simply **too many requests per tick**. With `playbackWindowAhead = 4` and 1 clip, each tick generates 5 requests. At 30fps playback, that's 150 requests/second. The decoder has 16 lanes, so each lane handles ~9 requests/second. But each decode takes ~10-30ms for a hardware-decoded frame. So each lane can process ~30-100 frames/second.

**The math works**: 150 requests/second across 16 lanes = ~9 requests/second/lane. Each lane can do 30-100 frames/second. So the decoder should keep up.

**But** — the `cancelDecoderBeforeThrottled()` call cancels queued requests before `keepFromFrame`. With `keepWindow = 16`, at playhead 100, it cancels everything before frame 84. But the queue might have requests for frames 85-99 from previous ticks that haven't been processed yet. These get cancelled. Then in subsequent ticks, new requests for those frames might be re-issued if the playhead hasn't advanced past them yet.

**This is the real churn**: Requests for frames 85-99 get issued in tick N, cancelled in tick N+1 (because the playhead moved and `cancelBeforeFrame` advanced), then re-issued in tick N+2 (if the window still includes them).

---

## The Fundamental Architectural Fix Needed

### Fix 1: Separate "Keep Window" from "Cancel Before"

Currently, `cancelDecoderBeforeThrottled()` uses `keepWindow` to determine what to cancel:
```cpp
cancelDecoderBeforeThrottled(info.playbackPath,
    qMax<int64_t>(0, canonicalFrame - keepWindow));
```

This means frames within the keep window are protected from cancellation. But the keep window is also used for buffer retention. These should be separate concerns:
- **Buffer keep window**: How many frames behind the playhead to keep in the buffer (for presentation)
- **Cancel-before threshold**: How far behind the playhead to cancel queued requests

The cancel-before threshold should be **much smaller** than the keep window, because:
- Cancelling a queued request for frame 85 when the playhead is at 100 means that frame will never be shown (it's 15 frames behind)
- But cancelling a queued request for frame 98 when the playhead is at 100 means it might still be useful (it's only 2 frames behind)

**Recommendation**: Cancel-before should be `playhead - 2` (not `playhead - keepWindow`). This prevents cancellation of frames that are still near the playhead.

### Fix 2: Rate-Limit Request Dispatch

Instead of issuing requests for the entire window every tick, only issue requests for frames that aren't already pending or in-flight. The current code already does this check (`m_pendingVisibleRequests.contains(key)`), but it doesn't check what's actually in the decoder's queue.

**Recommendation**: Add a method to query the decoder's pending queue for a specific file+frame, and skip dispatch if the frame is already queued in the decoder.

### Fix 3: Increase Prefetch Timeout

Visible requests have a 30-second timeout. Prefetch requests have a 12-second timeout. With the high request volume, prefetch requests might time out before they're processed, causing unnecessary null callbacks.

**Recommendation**: Increase prefetch timeout to match visible timeout, or remove timeouts entirely for queued requests (they're already bounded by the cancel-before mechanism).

### Fix 4: Remove the Cancel-Before Throttle Entirely During Playback

The cancel-before mechanism was designed for seeking, where you want to quickly cancel old requests when jumping to a new position. During smooth playback, the playhead advances by 1 frame per tick, and the cancel-before threshold advances by 1 frame per tick. This means every tick cancels at most 1 frame's worth of queued requests.

**But** — if a frame was requested 10 ticks ago and is still queued (hasn't been picked up by a worker), it gets cancelled. Then in the next tick, if the window still includes that frame, it gets re-requested. This creates a cancel-reissue loop.

**Recommendation**: During playback, don't cancel requests for frames that are still within the playback window. Only cancel requests for frames that are definitively behind the playhead by more than `maxPresentationPastFrameDelta`.

### Fix 5: Single Dispatch Point

The dual-path architecture (TimelineCache + PlaybackFramePipeline both issuing requests) is fundamentally redundant. During playback, only the pipeline path is active. During seeking/pausing, only the cache path is active. But they share the same `AsyncDecoder`, and the decoder has no way to distinguish between "this request is from the pipeline" and "this request is from the cache."

**Recommendation**: Route all visible decode requests through a single dispatch point that understands the current playback state and can make intelligent decisions about what to request and when.

---

## Summary of Architectural Issues

| # | Issue | Impact | Fix |
|---|-------|--------|-----|
| 1 | Cancel-before uses keep window instead of a tighter threshold | Cancels requests for frames still near the playhead | Use `playhead - 2` instead of `playhead - keepWindow` |
| 2 | No rate limiting on request dispatch | Generates more requests than the decoder can handle | Check decoder queue before dispatching |
| 3 | Prefetch timeout too short (12s vs 30s) | Prefetch requests time out before processing | Match visible timeout or remove |
| 4 | Cancel-before during smooth playback creates cancel-reissue loop | Same frames get cancelled and re-requested repeatedly | Skip cancel-before for frames still in window |
| 5 | Dual dispatch paths with no coordination | Redundant requests, no unified view of pending work | Single dispatch point |

---

## Proposed Implementation

### Phase 1: Fix Cancel-Before Threshold

In `schedulePlaybackWindow()` and `TimelineCache::requestFrame()`, change the cancel-before calculation to use a tighter threshold:

```cpp
// Instead of:
cancelDecoderBeforeThrottled(info.playbackPath,
    qMax<int64_t>(0, canonicalFrame - keepWindow));

// Use:
const int64_t cancelBeforeFrame = qMax<int64_t>(0, canonicalFrame - debugMaxPresentationPastFrameDelta() - 1);
cancelDecoderBeforeThrottled(info.playbackPath, cancelBeforeFrame);
```

This ensures only frames that are definitively behind the presentation window get cancelled.

### Phase 2: Rate-Limit Dispatch

In `schedulePlaybackWindow()`, before dispatching, check if the decoder already has a queued request for this file+frame:

```cpp
// After the pending check, before dispatch:
if (m_decoder->hasQueuedRequest(info.playbackPath, targetFrame)) {
    continue; // Already queued in decoder, skip
}
```

This requires adding a `hasQueuedRequest()` method to `AsyncDecoder`.

### Phase 3: Increase Prefetch Timeout

Change prefetch timeout from 12000 to 30000 to match visible timeout.

### Phase 4: Skip Cancel-Before for In-Window Frames

In `cancelDecoderBeforeThrottled()`, skip cancellation for frames that are still within the playback window:

```cpp
// Only cancel frames that are definitively behind
if (keepFromFrame < playheadFrame - maxPresentationPastFrameDelta) {
    m_decoder->cancelForFileBefore(playbackPath, keepFromFrame);
}
```

### Phase 5: Single Dispatch Point (Long-Term)

Create a unified `FrameRequestDispatcher` that:
- Knows the current playback state (playing, paused, seeking)
- Maintains a single set of pending requests
- Routes requests to the appropriate consumer (pipeline buffer or cache)
- Applies rate limiting and backpressure
