# Professionalization Plan

This tracks the work needed to turn the current live Pipeline/Vulkan preview debugging path into a maintainable, production-quality implementation.

## Pipeline Diagnostics

- Replace inferred Pipeline tab labels with structured snapshots emitted at real pipeline boundaries.
- Include all draw-gate facts in the live pipeline model:
  - `handoffAttempted`
  - `handoffSuccess`
  - `sampledFrameReady`
  - `textureDrawCount`
  - `fallbackDrawCount`
  - `exactFrameAvailable`
  - `requestedSourceFrame`
  - `presentedSourceFrame`
  - `frameLag`
  - `pendingVisibleRequestAgeMs`
  - `decoderCompletionReason`
- Make each stage distinguish between actual image data and diagnostic fallback imagery.
- Keep thumbnails and hover previews as presentation-only transforms; never let UI cropping imply the underlying frame is correct.
- Add a clear stage state enum: `ready`, `approximate`, `missing`, `blocked`, `fallback`, `error`.

## UI Architecture

- Split the Pipeline tab into separate components:
  - runtime diagnostic model adapter
  - Qt list rendering
  - image preview popup
  - polling/refresh controller
- Avoid refreshing the whole inspector during playback.
- Keep Pipeline tab polling visible-only and rate-limited.
- Preserve scroll/hover state across refreshes without rebuilding unnecessary UI objects.
- Remove native Qt tooltips from pipeline rows when custom previews are active.

## Vulkan Preview

- Keep Vulkan preview readback direct from Vulkan memory; do not use Qt widget screenshots.
- Keep the swapchain readback path separate from the main draw path.
- Record readback format, orientation, and color conversion explicitly in diagnostics.
- Add a draw-gate diagnostic stage after decode and before present.
- Report target/fitted rects, transform MVP validity, descriptor-set validity, and sampled image dimensions.

## Decode And Cache Policy

- Replace the current brittle visible request policy with a documented latest-wins policy.
- Keep successfully decoded visible frames even if playback advanced while decoding, so approximate preview can catch up.
- Do not let one stale pending visible request block newer visible requests indefinitely.
- Track pending visible request age using one consistent clock domain.
- Revisit `kMaxVisibleBacklog = 1`; either justify it with tests or replace it with a bounded coalescing queue.
- Increase or make adaptive the Vulkan preview GPU cache budget; 64 MiB is too tight for 1080p hardware frames.
- Separate exact-frame readiness from approximate-frame usability in API and UI.

## Control Server And Telemetry

- Fix `/state` so it does not return stale/empty project state while `/profile` has correct live data.
- Add a `/pipeline` endpoint exposing the same structured snapshots used by the Pipeline tab.
- Add pending request debug entries to `/profile`:
  - key
  - frame number
  - age
  - generation
  - callback count
  - cancellation/drop reason
- Add decoder completion counters:
  - decoded exact
  - decoded approximate
  - canceled before decode
  - canceled after decode
  - timed out
  - queue rejected
- Make live diagnostics sufficient to determine whether a frame is blocked at decode, cache, handoff, shader, composite, or present.

## Tests

- Add unit tests for `TimelineCache::shouldForceVisibleRequestRetry()` using consistent wall-clock timestamps.
- Add tests proving a successfully decoded visible frame is retained even if playback advances during decode.
- Add tests for stale pending visible request replacement/coalescing.
- Add tests for exact vs approximate frame selection during playback.
- Add Vulkan diagnostic tests that validate stage metadata without requiring screenshot comparison.
- Add regression tests for Pipeline tab refresh behavior: visible-only polling, scroll preservation, hover preview stability.

## Performance Targets

- Prefer accuracy and lightweightness over ultra-low latency.
- Avoid full inspector refreshes during playback.
- Avoid CPU materialization unless explicitly required or selected as a fallback.
- Keep diagnostic polling bounded and cheap.
- Make any heavy debug view opt-in or visible-only.

## Acceptance Criteria

- During playback, the Pipeline tab updates live without full inspector refresh.
- The main preview either displays the current exact frame or reports a bounded approximate lag with a reason.
- A stale visible request cannot permanently block newer visible requests.
- The Pipeline tab can identify the failing stage without screenshots or filesystem probes.
- `/profile` and `/pipeline` expose enough information to debug preview failure from a running instance.
- Automated tests cover the retry/cancel/cache behavior that caused stale preview frames.
