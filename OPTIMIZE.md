# Optimization Plan

## Goal

Reduce the CPU and UI-thread cost of control-server profiling, especially while playback is active, without regressing the usefulness of `/profile` for diagnostics.

Primary hotspots from the live profiles:

- `DirectVulkanPreviewRenderer::imageFromReadback(...)`
- `__memmove_avx_unaligned_erms`
- `TranscriptTab::syncTableToPlayhead(...)`
- `QWidgetRepaintManager::paintAndFlush()`
- control-server profile cache refresh activity during playback

## Summary Of Findings

Two profiles were collected:

1. Baseline playback profile
2. Playback profile while repeatedly calling `GET /profile`

The `/profile` endpoint itself is fast on localhost, but it keeps profile demand active, which causes the control server to refresh the cached UI-thread profile during playback. That pulls in expensive preview readback, image copies, transcript table sync work, and repaint churn.

The endpoint is not slow because of HTTP or JSON serialization. It is slow because the act of polling it keeps expensive background refreshes alive.

## Implementation Order

1. Make `/profile` cheap by default
2. Remove diagnostic image readback from the default profile path
3. Remove transcript table row scans from the playback sync path
4. Reduce repaint churn triggered by profile-demand refresh
5. Re-profile and tune intervals and fallbacks

## Workstream 1: Make `/profile` Cheap By Default

### Objective

Stop ordinary `GET /profile` polling from forcing expensive UI-thread profile refresh during active playback.

### Files

- [control_server_worker.cpp](/home/julian/Documents/JCut/control_server_worker.cpp)
- [control_server_worker.h](/home/julian/Documents/JCut/control_server_worker.h)
- [control_server_worker_routes.cpp](/home/julian/Documents/JCut/control_server_worker_routes.cpp)
- [control_server_webpage.html](/home/julian/Documents/JCut/control_server_webpage.html)

### Changes

- Split profile reads into two modes:
  - cheap cached mode: default `GET /profile`
  - explicit live mode: `GET /profile?live=1` or `GET /profile/live`
- In cheap mode:
  - return `fastSnapshot()`
  - return the last cached profile snapshot if available
  - do not mark profile demand in a way that keeps the UI-thread refresh loop hot during playback
- In live mode:
  - allow a forced refresh from the UI thread
  - report when the response is stale, throttled, skipped, or timing out
- Rework `profileInDemand` logic so passive polling does not keep active playback in heavy refresh mode
- Clamp refresh cadence during playback:
  - either disable live refreshes during playback by default
  - or increase the minimum refresh interval substantially, for example `500-1000ms`
- Shorten the profile demand window so short polling bursts do not keep the refresh loop active for long

### Acceptance Criteria

- Repeated default `GET /profile` calls do not materially change playback CPU usage
- The endpoint still returns useful health, cache, and stale/live metadata
- Explicit live mode still works for debugging and clearly advertises freshness

## Workstream 2: Remove Diagnostic Image Readback From Default Profile Path

### Objective

Stop profile refresh from triggering or depending on full-frame CPU-side preview image construction unless explicitly requested.

### Files

- [direct_vulkan_preview_presenter.cpp](/home/julian/Documents/JCut/direct_vulkan_preview_presenter.cpp)
- [direct_vulkan_preview_presenter.h](/home/julian/Documents/JCut/direct_vulkan_preview_presenter.h)
- [opengl_preview_window_core.cpp](/home/julian/Documents/JCut/opengl_preview_window_core.cpp)
- [vulkan_preview_surface.cpp](/home/julian/Documents/JCut/vulkan_preview_surface.cpp)
- any profile snapshot builders that include preview diagnostic image state

### Changes

- Audit what data in the profile snapshot actually requires readback images
- For default profile snapshots:
  - include metadata only
  - image size, format, counters, timestamps, booleans, last successful readback age
- Do not generate `QImage` objects just so `/profile` can expose diagnostic preview state
- Gate diagnostic image capture behind an explicit flag, endpoint, or debug setting
- Rate-limit decoder diagnostic readback and preview readback aggressively when enabled
- Reuse readback buffers and any staging structures where possible
- If format conversion is still required in diagnostic mode:
  - keep it out of the steady-state control-server path
  - prefer delayed or on-demand conversion over always doing it during playback

### Acceptance Criteria

- Default `/profile` polling no longer makes `imageFromReadback(...)` and `memmove` dominant hotspots
- Diagnostic preview image capture remains available through an explicit opt-in path

## Workstream 3: Remove Transcript Table Row Scans From Playback Sync

### Objective

Take `QTableWidgetItem::data()` and row iteration out of the per-sync playback path.

### Files

- [transcript_tab.cpp](/home/julian/Documents/JCut/transcript_tab.cpp)
- [transcript_tab.h](/home/julian/Documents/JCut/transcript_tab.h)

### Changes

- Replace row-by-row skip/outside-cut checks in `syncTableToPlayhead(...)` with precomputed lookup data
- Build cached skip/outside-cut frame ranges whenever transcript table contents or follow ranges change
- Use binary search over cached ranges to detect whether the current source frame lies in a skipped/outside-cut region
- Keep existing follow-row logic, but avoid touching `QTableWidgetItem` during active playback sync
- Audit whether matching-row resolution can also use precomputed structures more consistently
- Preserve current manual-selection and follow-current-word behavior

### Acceptance Criteria

- `syncTableToPlayhead(...)` does not iterate table rows on each playback update
- `QTableWidgetItem::data()` no longer shows up as a significant hotspot in profile-driven playback
- Transcript follow behavior remains correct across seek, pause, resume, and skipped regions

## Workstream 4: Reduce Repaint Churn During Profile Refresh

### Objective

Prevent profile-demand refresh from indirectly causing unnecessary widget repaint and layout activity.

### Files

- [control_server_worker.cpp](/home/julian/Documents/JCut/control_server_worker.cpp)
- [opengl_preview_window_core.cpp](/home/julian/Documents/JCut/opengl_preview_window_core.cpp)
- [timeline_renderer.cpp](/home/julian/Documents/JCut/timeline_renderer.cpp)
- [transcript_tab.cpp](/home/julian/Documents/JCut/transcript_tab.cpp)
- any UI update sites touched by the profiling callback

### Changes

- Audit the profile callback and related playback sync code for:
  - `update()`
  - model writes
  - selection churn
  - layout invalidation
  - icon refreshes
- Only mutate UI state if values actually changed
- Debounce or suppress nonessential paint-triggering updates while playback is active
- If needed, separate “gather profiling data” from “refresh visible widgets”
- Ensure profiling snapshots read state without forcing cosmetic redraws

### Acceptance Criteria

- Default `/profile` polling does not materially increase `QWidgetRepaintManager::paintAndFlush()` cost
- Timeline and tool widgets do not repaint just because profile data was requested

## Workstream 5: Tune Refresh Intervals, Fallbacks, And Behavior

### Objective

Make the control server resilient under playback load and reduce the chance that debugging tooling disturbs the app.

### Files

- [control_server_worker.cpp](/home/julian/Documents/JCut/control_server_worker.cpp)
- [control_server_worker.h](/home/julian/Documents/JCut/control_server_worker.h)
- [control_server_worker_routes.cpp](/home/julian/Documents/JCut/control_server_worker_routes.cpp)

### Changes

- Revisit:
  - `m_profileCacheFreshMs`
  - `m_profileRefreshIntervalMs`
  - `m_profileDemandWindowMs`
  - UI timeout and cooldown values
- Serve stale-but-labeled profile data rather than aggressively refreshing under load
- Expose freshness and throttling reasons clearly in the payload
- Prefer explicit freshness metadata over hidden refresh attempts

### Acceptance Criteria

- During active playback, the server favors stale cached data over intrusive live work
- Debug clients can tell whether they received fresh, stale, skipped, or throttled profile data

## Suggested Delivery Strategy

### Phase 1

- Implement Workstream 1
- Implement Workstream 2 enough to remove readback images from default `/profile`
- Re-profile playback plus `/profile` polling

Expected result:
- biggest immediate reduction in CPU cost

### Phase 2

- Implement Workstream 3
- Re-profile transcript-heavy playback scenarios

Expected result:
- UI-thread cost drops further, especially in transcript-follow mode

### Phase 3

- Implement Workstream 4
- Tune Workstream 5 parameters
- Re-profile and compare against the original captures

Expected result:
- smoother playback under tooling load, fewer incidental repaints

## Validation Plan

After each phase, collect and compare:

- baseline playback profile
- playback while polling default `GET /profile`
- playback while polling explicit live profile mode

Measure:

- total CPU impact on `jcut`
- share of samples in:
  - `imageFromReadback(...)`
  - `__memmove_avx_unaligned_erms`
  - `TranscriptTab::syncTableToPlayhead(...)`
  - `QTableWidgetItem::data(int) const`
  - `QWidgetRepaintManager::paintAndFlush()`
- `/profile` response freshness and latency
- playback smoothness and whether underruns or visible UI hitching increase

## Risks

- making `/profile` cached-by-default may change assumptions in existing tooling
- removing default readback images may affect debug UIs that expect embedded visual diagnostics
- transcript follow logic is easy to regress if cached range invalidation is incomplete
- repaint reductions can accidentally hide legitimate UI updates if change detection is too aggressive

## Non-Goals

- broad renderer redesign
- changing playback architecture unrelated to measured hotspots
- optimizing unrelated decoder paths unless they remain hot after the targeted fixes

## Exit Criteria

This plan is complete when:

- default `/profile` polling no longer causes large playback CPU regressions
- preview readback/image-copy work is no longer dominant under ordinary profile polling
- transcript follow sync avoids table-row scans on the playback path
- repaint overhead under profile polling is materially reduced
- explicit live diagnostics remain available when intentionally requested
