# Large-File Refactor Plan

## Objective

Reduce high-churn source files to the repository's 1500-line hard cap, with a
preferred steady-state size of 800-1200 lines. Splits are by responsibility;
the first pass should move code without changing behavior or public APIs.

Baseline: 2026-07-10, using `python countlines.py`.

## Current Priorities

| Priority | File | Lines | Main problem | Initial target |
|---|---:|---:|---|---|
| P0 | `tracks.cpp` | 4827 | assignment, diagnostics, previews, and UI actions are mixed | <1000 |
| P0 | `offscreen_vulkan_renderer_backend.cpp` | 4826 | one private backend owns every Vulkan stage | <1200/file |
| P0 | `speakers_tab.cpp` | 4117 | document, refresh, tables, generation, and status logic are mixed | <1000 |
| P0 | `audio_engine.cpp` | 4092 | device control, scheduling, decoding, stretching, and mixing are mixed | <1200/file |
| P0 | `jcut_imgui_main.cpp` | 3901 | application shell, platform/Vulkan setup, workers, and panels are mixed | <800 |
| P1 | `vulkan_facedetections_offscreen_runner.cpp` | 3399 | CLI orchestration and the full processing pipeline are one function | <800 |
| P1 | `direct_vulkan_preview_window.cpp` | 3307 | window, renderer lifecycle, readback, and frame submission are mixed | <1000 |
| P1 | `editor_shared_keyframes.cpp` | 3097 | generic keyframes and speaker-framing runtime are coupled | <1000 |
| P1 | `vulkan_preview_surface.cpp` | 2865 | preview lifecycle and rendering responsibilities are broad | <1000 |
| P1 | `inspector_pane.cpp` | 2812 | layout, bindings, state sync, and actions remain mixed | <1000 |

The remaining files above 1500 lines form the P2 backlog. Re-run the report
after P0/P1 because extractions may make several of them shrink or reveal
shared code: `vulkan_detector_frame_handoff.cpp`,
`facedetections_continuity_artifacts.cpp`, `speakers_tab_interactions_ai.cpp`,
`control_server_worker_routes.cpp`, `editor.cpp`, `editor_media_tools.cpp`,
`editor_ai_integration.cpp`, `transcript_tab.cpp`, `vulkan_text_renderer.cpp`,
`editor_tabs.cpp`, `control_server_worker_routes_ui.cpp`,
`speakers_tab_interactions.cpp`, `editor_render_tools.cpp`,
`transcript_tab_document.cpp`, `sam3_run.py`, `vulkan_resources.cpp`,
`render_export.cpp`, `editor_shared_transcript.cpp`, `timeline_cache.cpp`,
`speakers_tab_facedetections_generation.cpp`, `imgui_preview_window.cpp`, and
`editor_playback.cpp`.

## Phase 0: Guardrails

1. Record the baseline in CI and warn at 1200 lines, fail when a changed file
   grows beyond 1500 lines. Grandfather current oversized files but reject
   line-count growth.
2. Build and test before every extraction. Add new translation units to the
   owning CMake target in the same commit as the move.
3. Keep each extraction move-only. Do naming, ownership, or data-flow changes
   only after the split compiles and focused tests pass.
4. Prefer private free functions or internal implementation objects over
   expanding public headers. Put cross-translation-unit private declarations
   in a narrowly scoped `*_internal.h`.

## Phase 1: Speaker and Track Domain

Do these together because `tracks.cpp` implements `SpeakersTab`, and splitting
either file independently could create a second round of churn.

### `tracks.cpp`

Extract in this order:

1. `speakers_tab_track_avatars.cpp`: avatar decoding, representative frames,
   continuity strips, hover previews, and tooltip rendering (roughly current
   lines 499-1347).
2. `speakers_tab_track_diagnostics.cpp`: paths/raw-detections panels, debug
   snapshots, cache clearing, and deferred diagnostic refresh (roughly
   706-1949, excluding avatar functions already moved).
3. `speakers_tab_section_assignments.cpp`: contiguous-section lookup,
   assignment, persistence, rotation, and deassignment (roughly 1951-3268).
4. `speakers_tab_track_actions.cpp`: assign/deassign commands, preview face-box
   actions, context menus, and batch anchor application (roughly 3416-4285).
5. `speakers_tab_track_picker.cpp`: candidate list, picker dialogs, and avatar
   refresh actions (roughly 4286-end).

Keep only small shared JSON/key helpers in `tracks.cpp`, then rename it to
`speakers_tab_tracks_core.cpp` once references are stable.

### `speakers_tab.cpp`

Extract:

1. `speakers_tab_document.cpp`: transcript load/save/flush and loaded-document
   application.
2. `speakers_tab_sections_table.cpp`: section row construction, selection,
   playhead synchronization, and incremental table refresh.
3. `speakers_tab_speakers_table.cpp`: speaker-table population, display labels,
   and speaker lookup/navigation.
4. `speakers_tab_facedetections_actions.cpp`: generate/delete/view actions;
   merge with the existing file of that name instead of creating a duplicate.
5. `speakers_tab_status.cpp`: tracking/generator status polling and label
   updates.

Leave construction and top-level `refresh()` dispatch in `speakers_tab.cpp`.
Before moving helpers, check `speakers_tab_internal.h` for an existing owner;
deduplicate repeated section/track JSON helpers across these two source files.

Verification: speaker/section selection, assign and deassign, face-box click,
transcript save/reload, generation cancellation, and avatar refresh tests.

## Phase 2: Audio Engine

Keep `AudioEngine`'s public API stable and move member definitions into:

1. `audio_engine_device.cpp`: initialization, shutdown, PortAudio callback,
   transport, clock, volume, and output status.
2. `audio_engine_schedule.cpp`: clip/range snapshots, timeline-to-source
   mapping, decode queues, prioritization, and readiness checks.
3. `audio_engine_decode.cpp`: `decodeClipAudio`, cache insertion, and
   `decodeLoop`.
4. `audio_engine_time_stretch.cpp`: job state, Rubber Band settings,
   precompute, sidecar loading, and stretched-cache construction. Reuse the
   existing `audio_time_stretch*.cpp` abstractions where they already own a
   responsibility.
5. `audio_engine_mix.cpp`: speech-range blending, crossfades, dynamics,
   `mixChunk`, and `mixLoop`.
6. `audio_ring_buffer.cpp`: ring-buffer implementation if it is useful outside
   the engine; otherwise keep it private in the device file.

Verification: unit tests for time/sample mapping and range blending, then
play/seek/rate-change tests, cache miss/failure tests, and an export audio
comparison. Run a thread sanitizer build if supported; this split crosses
mutex and worker-thread boundaries.

## Phase 3: Vulkan Rendering

### `offscreen_vulkan_renderer_backend.cpp`

First replace the monolithic `OffscreenVulkanRendererPrivate` definition with
a private declaration in `offscreen_vulkan_renderer_internal.h`. Then extract
cohesive methods into:

1. `offscreen_vulkan_renderer_device.cpp`: instance/device/queue, memory type,
   command pools, and teardown.
2. `offscreen_vulkan_renderer_resources.cpp`: images, buffers, descriptors,
   samplers, pipelines, and resource caches.
3. `offscreen_vulkan_renderer_composite.cpp`: frame layer composition, masks,
   overlays, grading, and draw submission.
4. `offscreen_vulkan_renderer_readback.cpp`: BGRA/YUV readback and staging.
5. `offscreen_vulkan_renderer_nv12.cpp`: NV12 conversion and CUDA external
   memory transfer.
6. Keep the public facade and high-level `renderFrame` orchestration in the
   original file.

This is the highest-risk split. Do not redesign resource ownership during the
move. Verify CPU/Vulkan pixel parity, headless export, NV12/YUV paths, resize,
device loss, repeated initialize/shutdown, and validation-layer output.

### `direct_vulkan_preview_window.cpp`

Extract class declarations to `direct_vulkan_preview_internal.h`, then move:

1. renderer initialization/release and device loss to
   `direct_vulkan_preview_lifecycle.cpp`;
2. clip/title resource caches to `direct_vulkan_preview_resources.cpp`;
3. swapchain/decoder readback to `direct_vulkan_preview_readback.cpp`;
4. `startNextFrame` composition and submission to
   `direct_vulkan_preview_frame.cpp`;
5. window facade functions to `direct_vulkan_preview_window_api.cpp`.

Reuse the already split audio, presenter, geometry, interaction, transcript,
and overlay-rendering files; do not move those concerns back into the core.

## Phase 4: ImGui Application Shell

Split `jcut_imgui_main.cpp` into:

1. `jcut_imgui_platform.cpp`: X11 input and Vulkan/ImGui platform setup.
2. `jcut_imgui_preferences.cpp`: font and UI preference load/save.
3. `jcut_imgui_runtime_control.cpp`: runtime snapshots, screenshots, and
   playhead control.
4. `jcut_imgui_workers.cpp`: preview/export worker orchestration.
5. `jcut_imgui_media_panel.cpp`, `jcut_imgui_preview_panel.cpp`,
   `jcut_imgui_timeline_panel.cpp`, and `jcut_imgui_inspector_panel.cpp`.
6. Keep startup, the event loop, top-level layout, and shutdown in
   `jcut_imgui_main.cpp`.

Move `ShellState`, `ShellLayout`, and platform state into
`jcut_imgui_internal.h`; avoid exposing them as application-wide APIs.

Verification: launch/shutdown, preference persistence, media import,
play/seek/edit, preview refresh, export/cancel, and runtime-control smoke tests.

## Phase 5: Face-Detection Runner and Keyframes

### `vulkan_facedetections_offscreen_runner.cpp`

Break the current 3200-line orchestration function into stages:

1. `vulkan_facedetections_offscreen_cli.cpp`: argument conversion and option
   validation, building on the existing options module.
2. `vulkan_facedetections_offscreen_pipeline.cpp`: top-level stage sequencing.
3. `vulkan_facedetections_offscreen_decode.cpp`: input/decode and frame timing.
4. `vulkan_facedetections_offscreen_detection.cpp`: batching and detector
   invocation.
5. `vulkan_facedetections_offscreen_tracking.cpp`: continuity/tracking handoff.
6. `vulkan_facedetections_offscreen_artifacts.cpp`: checkpoint, resume, and
   output coordination, delegating actual I/O to the existing artifact and
   checkpoint modules.

Verification: identical CLI errors and exit codes, clean run, resume after
interruption, cancellation, empty input, and artifact JSON comparison.

### `editor_shared_keyframes.cpp`

Extract by keyframe family:

1. `editor_keyframes_normalize.cpp`: normalization for transform, grading,
   opacity, and title keyframes.
2. `editor_keyframes_transform.cpp`: generic transform interpolation and
   composition.
3. `editor_keyframes_grading.cpp`, `editor_keyframes_opacity.cpp`, and
   `editor_keyframes_titles.cpp`: family-specific evaluation.
4. `editor_speaker_framing_continuity.cpp`: artifact lookup, cache warm-up,
   tracking samples, smoothing, and runtime preparation.
5. `editor_speaker_framing_keyframes.cpp`: target retargeting and speaker-frame
   transform evaluation.

Generic keyframe code must not depend on face-detection artifacts after this
split. Add table-driven boundary/interpolation tests before moving evaluation
logic, including duplicate frames, empty tracks, fractional positions, source
lock, and rotation wraparound.

## Phase 6: P1/P2 Sweep

Apply the same seams to `vulkan_preview_surface.cpp` and `inspector_pane.cpp`,
then regenerate the line report. Tackle remaining files in descending order,
except where files share a domain: complete one domain together to avoid
repeated internal-header and CMake churn. Python files use the same 1500-line
cap but should be split into importable option, pipeline, I/O, and reporting
modules rather than mechanically divided scripts.

## Per-File Execution Checklist

1. Identify functions and state for one destination; document its ownership in
   the PR/commit description.
2. Add focused characterization tests if the behavior is not already covered.
3. Add the destination to the correct CMake target.
4. Move declarations and definitions without semantic edits.
5. Build the narrowest target, run focused tests, then run the full suite.
6. Compare warnings, runtime diagnostics, artifacts, or rendered output as
   appropriate to the domain.
7. Commit one responsibility extraction at a time; do not combine unrelated
   cleanup.
8. Re-run `python countlines.py` and update this baseline after each phase.

## Definition of Done

- No changed source or header exceeds 1500 lines; active core files aim for
  800-1200 lines.
- Each destination has one clear responsibility and a narrow private boundary.
- Public APIs and observable behavior are unchanged for move-only phases.
- Build, focused tests, and the full test suite pass.
- Rendering/audio/artifact outputs have domain-appropriate parity evidence.
- The size check prevents the files from growing back into monoliths.
