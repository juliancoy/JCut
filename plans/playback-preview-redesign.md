# Playback Preview Redesign: Unified Frame Dispatch

## Problem Summary

The current architecture has two independent paths for requesting video frames during playback:

```
VulkanPreviewSurface / OpenGL PreviewWindow
  |
  +---> TimelineCache::requestFrame()     [cache path - non-playback]
  |       Issues decode requests directly to AsyncDecoder
  |       Maintains its own pending set, diagnostics, callbacks
  |
  +---> PlaybackFramePipeline::requestFramesForSample()  [playback path]
          Issues decode requests directly to AsyncDecoder
          Maintains its OWN pending set, diagnostics, callbacks
          Has its OWN buffer (PlaybackBuffer)
```

Both paths issue requests to the same `AsyncDecoder`, but:
- They maintain **separate pending request sets** — the decoder has no way to know "this frame is already being decoded for another consumer"
- They have **separate cancel-before logic** — both call `cancelDecoderBeforeThrottled()` independently
- They have **separate diagnostics** — you can't see the unified picture of what's pending
- The `AsyncDecoder`'s `collectSupersededRequests()` treats requests from both paths identically, so a pipeline request can supersede a cache request and vice versa

## Design: Unified Frame Dispatcher

### New Class: `FrameDispatcher`

```
                    +------------------+
                    |  FrameDispatcher |
                    +------------------+
                    |                  |
                    |  - pending set   |
                    |  - rate limiter  |
                    |  - cancel logic  |
                    |  - diagnostics   |
                    |  - trace buffer  |
                    +--------+---------+
                             |
                    +--------+---------+
                    |  AsyncDecoder    |
                    +------------------+
```

The `FrameDispatcher` replaces the request-dispatch logic currently split across `TimelineCache` and `PlaybackFramePipeline`. It provides:

1. **Single `requestFrame()` entry point** — Both `TimelineCache` and `PlaybackFramePipeline` call this instead of going directly to `AsyncDecoder`
2. **Unified pending set** — One place to check if a frame is already being decoded
3. **Rate limiting** — Configurable max requests per tick, backpressure when decoder is saturated
4. **Smart cancel-before** — Only cancels frames definitively behind the playhead, not frames still in the window
5. **Unified diagnostics** — Single source of truth for pending requests, completion rates, wait times
6. **Callback routing** — Routes completed frames to the appropriate consumer (pipeline buffer or cache)

### Data Flow

```
VulkanPreviewSurface::refreshVisibleFrames()
  |
  +---> [playback active?]
  |       YES:
  |         PlaybackFramePipeline::requestFramesForSample()
  |           |
  |           +---> FrameDispatcher::requestFrame(path, frame, kind, callback)
  |                   |
  |                   +---> [already pending?] → chain callback, return
  |                   +---> [rate limit hit?] → return false (caller retries later)
  |                   +---> AsyncDecoder::requestFrame()
  |
  |       NO (seeking/paused):
  |         TimelineCache::requestFrame()
  |           |
  |           +---> [cache hit?] → return cached frame
  |           +---> FrameDispatcher::requestFrame(path, frame, kind, callback)
  |                   |
  |                   +---> [already pending?] → chain callback, return
  |                   +---> AsyncDecoder::requestFrame()
```

### Completion Flow

```
AsyncDecoder completes a frame
  |
  +---> FrameDispatcher::onFrameComplete(path, frame, kind)
          |
          +---> [kind == Visible?]
          |       YES:
          |         Route to PlaybackFramePipeline::onFrameReady(frame)
          |         Route to TimelineCache::onFrameReady(frame)
          |       NO (prefetch):
          |         Route to PlaybackFramePipeline::onFrameReady(frame)
          |
          +---> Invoke all chained callbacks
```

## Detailed Design

### FrameDispatcher Class

```cpp
class FrameDispatcher : public QObject {
    Q_OBJECT
public:
    struct PendingRequest {
        QString filePath;
        int64_t frameNumber = 0;
        int priority = 0;
        DecodeRequestKind kind = DecodeRequestKind::Prefetch;
        QVector<std::function<void(FrameHandle)>> callbacks;
        qint64 requestedAtMs = 0;
        uint64_t sequenceId = 0;
    };

    // Single entry point for all frame requests
    // Returns true if the request was accepted (queued or coalesced)
    bool requestFrame(const QString& filePath,
                      int64_t frameNumber,
                      int priority,
                      int timeoutMs,
                      DecodeRequestKind kind,
                      std::function<void(FrameHandle)> callback);

    // Called by AsyncDecoder when a frame completes
    void onFrameDecoded(const QString& filePath,
                        int64_t frameNumber,
                        FrameHandle frame);

    // Cancel requests before a given frame for a file
    void cancelBefore(const QString& filePath, int64_t keepFromFrame);

    // Query methods
    bool isPending(const QString& filePath, int64_t frameNumber) const;
    bool isNearbyPending(const QString& filePath, int64_t frameNumber, int slack) const;
    int pendingCount() const;
    int pendingCountForFile(const QString& filePath) const;

    // Rate limiting
    void setMaxRequestsPerTick(int max);
    int maxRequestsPerTick() const;

    // Diagnostics
    QJsonObject diagnosticsSnapshot() const;
    QJsonArray pendingRequestsSnapshot() const;

signals:
    void frameAvailable(const QString& filePath, int64_t frameNumber);

private:
    struct PendingKey {
        QString filePath;
        int64_t frameNumber;
        bool operator==(const PendingKey& o) const {
            return filePath == o.filePath && frameNumber == o.frameNumber;
        }
        uint hash() const {
            return qHash(filePath) ^ qHash(frameNumber);
        }
    };

    mutable QMutex m_mutex;
    QHash<PendingKey, PendingRequest> m_pending;
    QHash<QString, int64_t> m_latestTargets;  // per-file latest target frame
    int m_requestsThisTick = 0;
    int m_maxRequestsPerTick = 8;

    // Cancel-before throttle state
    QHash<QString, int64_t> m_lastCancelKeepFromByPath;
    QHash<QString, qint64> m_lastCancelAtMsByPath;

    // Diagnostics
    std::atomic<uint64_t> m_totalDispatched{0};
    std::atomic<uint64_t> m_totalCoalesced{0};
    std::atomic<uint64_t> m_totalRateLimited{0};
    std::atomic<uint64_t> m_totalCancelled{0};
    std::atomic<uint64_t> m_totalCompleted{0};
};
```

### Integration Points

#### 1. `TimelineCache` Changes

- Remove direct `m_decoder->requestFrame()` call from `TimelineCache::requestFrame()`
- Replace with `m_dispatcher->requestFrame()`
- Keep cache hit logic (check cache first, return cached frame if hit)
- Keep callback chaining (multiple callers for same frame)
- Remove `cancelDecoderBeforeThrottled()` call (now in dispatcher)
- Remove `dropObsoletePendingVisibleRequestsLocked()` (now in dispatcher)
- Remove `m_pendingVisibleRequests` and `m_pendingPrefetchRequests` (now in dispatcher)
- Remove `m_visibleDecodeDiagnostics` (now in dispatcher)
- Keep `m_latestVisibleTargets` for force-retry logic (or move to dispatcher)

#### 2. `PlaybackFramePipeline` Changes

- Remove direct `m_decoder->requestFrame()` call from `schedulePlaybackWindow()`
- Replace with `m_dispatcher->requestFrame()`
- Remove `m_pendingVisibleRequests` and `m_pendingPrefetchRequests` (now in dispatcher)
- Remove `cancelDecoderBeforeThrottled()` call (now in dispatcher)
- Remove `m_latestVisibleTargets` (now in dispatcher)
- Remove `m_decodeDiagnostics` (now in dispatcher)
- Remove `m_lastCancelKeepFromByPath` and `m_lastCancelAtMsByPath` (now in dispatcher)
- Keep `PlaybackBuffer` for frame storage
- Keep `onFrameReady()` for buffer insertion
- Keep `recordFrameTraceEvent()` for tracing (or move to dispatcher)

#### 3. `AsyncDecoder` Changes

- Remove `collectSupersededRequests()` — superseding/coalescing is now in the dispatcher
- The decoder becomes a pure worker pool: accept requests, decode frames, return results
- No more queue management based on frame proximity — that's the dispatcher's job
- Keep priority-based queue ordering
- Keep cancel-before (called by dispatcher)

#### 4. `VulkanPreviewSurface` / `OpenGL PreviewWindow` Changes

- No direct changes needed — they already go through `TimelineCache` and `PlaybackFramePipeline`
- The `pendingNearby` check in `evaluatePreviewVisibleRequest()` can be simplified — just check the dispatcher

## Implementation Plan

### Phase 1: Create FrameDispatcher class

1. Create `frame_dispatcher.h` and `frame_dispatcher.cpp`
2. Implement the core data structures (pending set, rate limiter, cancel logic)
3. Implement `requestFrame()` with coalescing and rate limiting
4. Implement `onFrameDecoded()` for callback routing
5. Implement diagnostics snapshot

### Phase 2: Integrate with TimelineCache

1. Add `FrameDispatcher*` member to `TimelineCache`
2. Replace `m_decoder->requestFrame()` with `m_dispatcher->requestFrame()`
3. Remove redundant pending tracking from TimelineCache
4. Remove redundant cancel-before logic from TimelineCache
5. Route completed frames through dispatcher

### Phase 3: Integrate with PlaybackFramePipeline

1. Add `FrameDispatcher*` member to `PlaybackFramePipeline`
2. Replace `m_decoder->requestFrame()` with `m_dispatcher->requestFrame()`
3. Remove redundant pending tracking from PlaybackFramePipeline
4. Remove redundant cancel-before logic from PlaybackFramePipeline
5. Route completed frames through dispatcher

### Phase 4: Simplify AsyncDecoder

1. Remove `collectSupersededRequests()` — superseding is now in dispatcher
2. Keep priority queue, cancel-before, worker pool
3. The decoder becomes a simpler "decode this frame" engine

### Phase 5: Clean up and test

1. Remove dead code from TimelineCache and PlaybackFramePipeline
2. Update REST API diagnostics to use dispatcher
3. Update frame trace to use dispatcher
4. Test playback, seeking, pausing

## Risk Assessment

| Risk | Impact | Mitigation |
|------|--------|------------|
| New class introduces bugs | High | Keep old code paths as fallback initially, test thoroughly |
| Performance regression from additional abstraction layer | Medium | Dispatcher is lightweight (hash lookups), no heap allocations in hot path |
| TimelineCache and PlaybackFramePipeline have different callback semantics | Medium | Dispatcher handles both uniformly — callbacks are invoked on frame completion |
| AsyncDecoder's collectSupersededRequests is tightly coupled to queue management | Medium | Move superseding logic to dispatcher, keep queue management in decoder |
| OpenGL and Vulkan paths have different frame selection logic | Low | Both go through same dispatcher, frame selection is separate |

## Files to Create/Modify

### New Files
- `frame_dispatcher.h` — FrameDispatcher class declaration
- `frame_dispatcher.cpp` — FrameDispatcher implementation

### Modified Files
- `timeline_cache.h` — Add FrameDispatcher member, remove redundant members
- `timeline_cache_requests.cpp` — Route through dispatcher
- `playback_frame_pipeline.h` — Add FrameDispatcher member, remove redundant members
- `playback_frame_pipeline.cpp` — Route through dispatcher
- `async_decoder.h` — Remove collectSupersededRequests declaration
- `async_decoder.cpp` — Remove collectSupersededRequests implementation, simplify
- `vulkan_preview_surface.cpp` — Pass dispatcher to pipeline and cache
- `opengl_preview_window_pipeline.cpp` — Pass dispatcher to pipeline and cache
- `editor.cpp` — Create FrameDispatcher, wire to cache and pipeline
- `debug_controls.h` — Add dispatcher-related debug controls (rate limit)
- `debug_controls.cpp` — Implement dispatcher debug controls
- `control_server_worker_routes.cpp` — Update diagnostics to use dispatcher
