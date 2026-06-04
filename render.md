# Render Path

## Purpose

This document describes the professional render pathway for J-Cut preview and export.
It should be read with `synchronization.md`, which defines the thread, queue, frame
lifetime, and GPU dependency contracts. `render.md` describes the render stages and
the allowed data flow between them.

The interactive preview path is the authority for real-time presentation. Offline
export uses the same timeline mapping and composition concepts, but it targets an
offscreen frame and then hands the result to the encoder.

## Core Rule

There is one async decoder object per render pipeline instance.

That decoder is not accessed through an additional central dispatcher. The render
owners decide what they need, and `AsyncDecoder` owns only decode execution:
worker lanes, per-file decoder contexts, hardware-device references, request
priority, cancellation, and decode diagnostics.

Preview code must not add another layer that pretends to own time, pending-visible
state, cache residency, or callback thread delivery. Those responsibilities already
belong to the components named in `synchronization.md`.

## Interactive Preview Path

1. `EditorWindow` owns the playback clock and publishes the current timeline sample.
2. `VulkanPreviewSurface` receives that sample and maps active clips to source-frame
   requests using trim, playback rate, source FPS, and render-sync markers.
3. `TimelineCache` serves paused, scrub, seek, and cache-backed visible requests.
   It owns cache lookup, cache mutation, playback-buffer insertion, pending-visible
   diagnostics, and queued callback delivery back to the cache owner thread.
4. During active direct-Vulkan video playback, `PlaybackFramePipeline` owns the
   bounded playback window and its playback buffer. It requests current and
   near-future source frames from `AsyncDecoder`, tracks local pending state, and
   queues decoded-frame mutation back to its owner thread.
   Only the current presentation sample is a visible request; future playback
   warmup for regular video is bounded prefetch.
5. `AsyncDecoder` decodes requested source frames and returns `FrameHandle` objects.
   Visible requests are not silently proximity-superseded; the exact request must
   complete or fail explicitly.
6. `VulkanPreviewSurface` builds `VulkanPreviewClipFrameStatus` from the selected
   frames, effects, transforms, grading, masks, speaker framing, and overlay state.
7. `DirectVulkanPreviewPresenter` renders the status list into the swapchain.

CPU readback and CPU image upload are diagnostic or compatibility paths only. The
direct Vulkan visible path requires hardware frames or external GPU texture payloads
unless a caller explicitly opts into a non-direct path.

## Export Path

`renderTimelineToFile(...)` creates its own `AsyncDecoder` for the export job. The
export path must not borrow the interactive preview decoder, cache, playback buffer,
or swapchain resources.

The chosen output FPS is the authority for export sampling. Export ranges are stored
on the edit timeline frame grid, but the export loop converts each output-frame PTS
to a fractional timeline position before source mapping, effects evaluation, and
speaker framing. A 60 fps export of a 30 fps edit range must render twice as many
output samples, not duplicate the 30 fps render loop after the fact.

For each output frame:

1. The export loop maps the output PTS to a fractional timeline position.
2. The fractional timeline position is mapped to active source frames.
3. The render backend decodes or reuses per-job decoder contexts for those source
   frames.
4. The selected backend composites the frame into an offscreen target.
5. Transcript/title overlays are applied through the render backend path.
6. The encoded frame is written to the output stream.

Export may use GPU rendering and direct NV12 conversion when available. If GPU export
is unavailable, the fallback must be explicit in the result message and diagnostics.

## Ownership Boundaries

- `EditorWindow`: playback clock and timeline sample.
- `VulkanPreviewSurface`: source mapping, visible request decisions, frame-status
  construction, overlay snapshot application.
- `TimelineCache`: cache storage, paused/scrub/seek visible requests, cache-backed
  playback fallback, cache diagnostics.
- `PlaybackFramePipeline`: active-playback request window and playback buffer.
- `AsyncDecoder`: decode queues, decoder contexts, hardware-device refs, cancellation,
  and decode timing diagnostics.
- `DirectVulkanPreviewPresenter`: swapchain presentation.
- Export render backend: offscreen render target and encoder handoff.

Do not move these responsibilities into a generic decode dispatcher unless the
synchronization contract is rewritten first and names the new owner, callback thread,
mutation boundaries, failure behavior, and regression tests.

## Failure Behavior

- Missing source mapping: refresh statuses with a missing-source reason; do not invent
  a frame.
- Missing visible frame: request the exact source frame and present only a bounded,
  allowed approximate frame.
- Output FPS mismatch: do not encode a different FPS by merely changing encoder
  time_base while continuing to render the fixed edit-frame loop.
- CPU-only visible direct-Vulkan payload: reject it for direct preview and report the
  payload mismatch.
- Late visible decode: cache or buffer it only if it is still inside the accepted
  presentation/residency window.
- Decoder cancellation or queue rejection: deliver a null frame through the owning
  component's queued callback path.
- GPU presenter failure: report the direct Vulkan failure explicitly; do not silently
  fall back to OpenGL.

## Diagnostics

Every render-path change must preserve enough diagnostics to prove the path in a
running instance:

- Current timeline sample and source-frame mapping.
- Visible request decision, backlog, pending/exact/displayable state, and block reason.
- Decoder timing and null-callback counters.
- Frame payload type: hardware, GPU texture, CPU image, or null.
- Cache/playback-buffer residency counts.
- Frame-status exact/approx/missing counts.
- Presenter/backend id and explicit fallback reason, if any.

These diagnostics are part of the render contract. A change that improves local
appearance but hides one of these facts is incomplete.
