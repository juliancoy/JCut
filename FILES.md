# FILES.md

This document describes what each **first-party** file group does and where it sits in the organizational paradigm.

Scope and conventions:
- Includes top-level project sources and scripts.
- Excludes vendored/generated trees (`external/`, `build/`, `ffmpeg-build/`, `ffmpeg-install/`, `rtaudio/` internals).
- "Position" uses layered architecture terms:
  - `L0` Entry/Binaries
  - `L1` Application Orchestration
  - `L2` Domain/State/Models
  - `L3` Rendering/Media Pipelines
  - `L4` UI/Presentation
  - `L5` Integration/Tooling/Diagnostics

## 1) Build and Entry Points

- `CMakeLists.txt` — Build graph, targets, dependency wiring. Position: `L0`.
- `build.sh` — Project bootstrap/build helper. Position: `L5`.
- `Makefile` — Convenience build aliases. Position: `L5`.
- `editor_main.cpp` — Main process entry, CLI flags, harness mode routing. Position: `L0`.
- `editor.cpp`, `editor.h` — Main editor shell object lifecycle. Position: `L1`.
- `backend_headless_compare_main.cpp` — Non-Qt benchmark/probe driver for backend compare. Position: `L0/L5`.

## 2) Application Orchestration (Editor Composition)

- `editor_setup.cpp` — Startup wiring and subsystem initialization. Position: `L1`.
- `editor_editor_pane.cpp` — Editor pane assembly/composition. Position: `L1/L4`.
- `editor_tabs.cpp` — Tab registration/wiring. Position: `L1/L4`.
- `editor_input.cpp` — Input/event routing. Position: `L1/L4`.
- `editor_playback.cpp` — Playback orchestration and clocks. Position: `L1/L3`.
- `editor_media_tools.cpp` — Import/media actions and tooling flows. Position: `L1/L5`.
- `editor_render_tools.cpp` — Render/export commands and progress UI hooks. Position: `L1/L3/L4`.
- `editor_profiling.cpp` — Runtime metrics surface in app. Position: `L1/L5`.
- `editor_inspector_bindings.cpp` — Inspector-to-state/action bindings. Position: `L1/L4`.
- `editor_pane.cpp`, `editor_pane.h` — Shared pane container behavior. Position: `L1/L4`.

## 3) Domain, Timeline, Project State

- `editor_shared.h` + `editor_shared*.cpp` — Canonical shared domain data and helpers (effects, media, timing, transcript, keyframes, sync). Position: `L2`.
- `clip_serialization.h/.cpp` — Timeline clip JSON serialization/deserialization. Position: `L2`.
- `project_state.h/.cpp` — Persisted project state IO and migration helpers. Position: `L2`.
- `project_manager.h/.cpp` — Project selection/open/save coordination. Position: `L1/L2`.
- `projects.h/.cpp` — Project discovery/current-project helpers. Position: `L2`.
- `timeline_container.h/.cpp` — Timeline container model and high-level behaviors. Position: `L2/L4`.
- `timeline_layout.h/.cpp` — Track/lane geometry calculations. Position: `L2/L4`.
- `timeline_renderer.h/.cpp` — Timeline view render logic. Position: `L4`.
- `timeline_widget.h` + `timeline_widget_*.cpp` — Timeline widget behavior split by concern (core, input, layout, model, paint, context menu). Position: `L4`.

## 4) Decode, Frame Pipeline, Caching

- `decoder_context.h/.cpp` — Decode context lifecycle and packet/frame flow. Position: `L3`.
- `decoder_ffmpeg_utils.h/.cpp` — FFmpeg helper routines and conversions. Position: `L3`.
- `decoder_image_io.h/.cpp` — Image decode/load helpers. Position: `L3`.
- `async_decoder.h/.cpp` — Background decode worker pool and async scheduling. Position: `L3`.
- `decode_trace.h/.cpp` — Decode instrumentation/tracing. Position: `L5`.
- `frame_handle.h/.cpp` — Frame container with GPU/CPU ownership semantics. Position: `L3`.
- `media_pipeline_shared.h/.cpp` — Shared playback/decode pipeline primitives. Position: `L3`.
- `timeline_cache.h/.cpp`, `timeline_cache_*.cpp`, `timeline_cache_seek_resync.h/.cpp` — Timeline frame cache, request queueing, storage, seek-resync behavior. Position: `L3`.
- `memory_budget.h/.cpp` — Memory pressure accounting and policy. Position: `L3/L5`.
- `playback_frame_pipeline.h/.cpp` — Playback frame flow integration. Position: `L3`.

## 5) Export / Render Pipeline

- `render.h` — Render request/progress/result contracts. Position: `L3`.
- `render_internal.h` — Internal render pipeline interfaces and shared internals. Position: `L3`.
- `render_export.cpp` — Video export orchestration (encode loop, progress, backend policy). Position: `L3`.
- `render_decode.cpp` — Render-time frame decode path. Position: `L3`.
- `render_audio.cpp` — Render-time audio mix/encode path. Position: `L3`.
- `render_codecs.cpp` — Encoder selection/options/pixel-format decisions. Position: `L3`.
- `render_stats.cpp` — Render stage accounting tables and worst-frame stats. Position: `L3/L5`.
- `render_gpu.cpp` — OpenGL offscreen GPU render implementation. Position: `L3`.
- `render_cpu_fallback.h/.cpp` — CPU color conversion fallback utilities (e.g., NV12 fill). Position: `L3`.
- `speaker_export_harness.h/.cpp` — Headless export harness CLI flow over project state. Position: `L0/L3/L5`.

## 6) Backend Selection and Preview Surface Abstraction

- `render_backend.h/.cpp` — Parse/resolve requested backend (`opengl`/`vulkan`/etc). Position: `L2/L3 policy`.
- `preview_surface.h` — Common preview surface interface. Position: `L3/L4 boundary`.
- `preview_surface_factory.h/.cpp` — Backend-aware preview surface construction and failure policy. Position: `L1/L3`.
- `preview_widget_factory.h/.cpp` — UI widget creation wrapper for preview surface. Position: `L1/L4`.

## 7) OpenGL Preview Path

- `opengl_preview.h` — OpenGL preview widget contract/state. Position: `L4/L3`.
- `opengl_preview_window_core.cpp` — Core preview lifecycle/timing/backend bridge state.
- `opengl_preview_window_gl.cpp` — GL drawing path and composition.
- `opengl_preview_window_pipeline.cpp` — Pipeline glue between cache/decode/render.
- `opengl_preview_window_overlay.cpp` — Overlay rendering.
- `opengl_preview_window_overlay_support.cpp` — Overlay utility logic.
- `opengl_preview_window_transcript.cpp` — Transcript overlay layout/render integration.
- `opengl_preview_window_transcript_gpu.cpp` — GPU transcript overlay path pieces.
- `opengl_preview_window_interaction.cpp` — Input/interaction behavior.
- `opengl_preview_debug.h/.cpp` — Debug probes and preview diagnostics.

All files above are Position: `L4` with `L3` rendering internals.

## 8) Vulkan Preview/Backend Scaffolding

- `vulkan_backend.h/.cpp` — QRhi Vulkan backend creation helper. Position: `L3 infrastructure`.
- `preview_renderer_backend.h/.cpp` — QRhi backend init (Vulkan/OpenGL/Null), compositor init. Position: `L3`.
- `direct_vulkan_preview_presenter.h/.cpp` — Direct `QVulkanWindow` swapchain presenter, device/surface preflight, and strict decode-readiness visualization. Position: `L4/L3`.
- `vulkan_preview_surface.h/.cpp` — Direct `QVulkanWindow` preview surface adapter and hardware-zero-copy decode readiness bridge. Position: `L4/L3`.

## 9) Compositing and Effects

- `gpu_compositor.h/.cpp` — QRhi-based GPU layer compositor abstraction. Position: `L3`.
- `gl_frame_texture_shared.h/.cpp` — GL texture sharing/lifecycle helpers. Position: `L3`.
- `visual_effects_shader.h` — Shader sources/constants for visual effects. Position: `L3`.
- `polygon_triangulation.h` — Geometry helper for correction polygons. Position: `L2/L3`.
- `titles.h/.cpp` — Title model evaluation/render primitives. Position: `L2/L3`.

## 10) UI Tabs and Panels

- `inspector_pane.h/.cpp`, `inspector_pane_*.cpp` — Inspector shell, tab collections, wiring, secondary tabs. Position: `L4`.
- `inspector_controller.h/.cpp` — Inspector state coordinator. Position: `L1/L4`.
- `explorer_pane.h/.cpp` — Explorer/asset navigation pane. Position: `L4`.
- `output_tab.h/.cpp` — Output settings/render controls tab. Position: `L4`.
- `profile_tab.h/.cpp` — Diagnostics/profile metrics tab. Position: `L4/L5`.
- `history_tab.h/.cpp` — History stack UI and actions. Position: `L4`.
- `clips_tab.h/.cpp`, `tracks_tab.h/.cpp`, `track_sidebar.h/.cpp` — Timeline structural editing tabs. Position: `L4`.
- `effects_tab.h/.cpp`, `opacity_tab.h/.cpp`, `grading_tab.h/.cpp`, `grading_tab_curve.cpp`, `grading_histogram_widget.h/.cpp`, `corrections_tab.h/.cpp`, `video_keyframe_tab.h/.cpp`, `keyframe_tab_base.h/.cpp`, `keyframe_table_shared.h/.cpp`, `table_tab_base.h/.cpp` — Visual/keyframe editing UI. Position: `L4`.
- `titles_tab.h/.cpp`, `transcript_tab.h/.cpp`, `transcript_tab_document.cpp`, `sync_tab.h/.cpp`, `speakers_tab*.cpp/.h` — Title/transcript/sync/speaker flows. Position: `L4`.
- `properties_tab.h/.cpp` — Generic property editor tab. Position: `L4`.

## 11) Transcript, Sync, Waveform, Audio Runtime Services

- `transcript_engine.h/.cpp` — Transcript parsing, sectioning, mapping utilities. Position: `L2/L3`.
- `transform_skip_aware_timing.h/.cpp` — Timing transform logic with skipped transcript regions. Position: `L2`.
- `waveform_service.h/.cpp` — Waveform generation/cache service. Position: `L3`.
- `audio_engine.h` — Audio playback engine interface/types. Position: `L3`.

## 12) Control Server / API Layer

- `control_server.h/.cpp` — Local control server lifecycle. Position: `L5`.
- `control_server_worker.h/.cpp` — Worker that executes API commands. Position: `L5`.
- `control_server_worker_routes.cpp`, `control_server_worker_routes_ui.cpp` — Endpoint routing and handlers. Position: `L5`.
- `control_server_http_utils.h/.cpp` — HTTP helper functions. Position: `L5`.
- `control_server_ui_utils.h/.cpp` — UI-state extraction helpers for API. Position: `L5`.
- `control_server_media_diag.h/.cpp` — Media diagnostics endpoints/helpers. Position: `L5`.
- `control_server_webpage.html` — Built-in diagnostics/control webpage. Position: `L5`.

## 13) AI / External Process Helpers

- `editor_ai_integration.cpp` — AI feature orchestration in main app. Position: `L1/L5`.
- `editor_ai_helpers.h/.cpp` — Utility wrappers for AI flows. Position: `L5`.
- `speaker_boxstream.py`, `speaker_face_candidates.py`, `sync_detector.py`, `docker_face_detector.py` — External ML/analysis helper scripts. Position: `L5`.
- `speaker_boxstream.dockerfile`, `syncnet.dockerfile` — Containerized toolchain definitions. Position: `L5`.

## 14) Test and Benchmark Harnesses

- `scripts/vulkan_parity_throughput.sh` — UI/control-server parity benchmark harness.
- `scripts/vulkan_parity_throughput_headless.sh` — Headless backend comparison wrapper.
- `scripts/vulkan_headless_export_compare.sh` — Export-based backend compare script.
- `tests/` and `tests/CMakeLists.txt` — Unit/integration tests for selected subsystems.
- `test*.cpp` (top-level) — ad hoc/legacy test programs.

Position: `L5`.

## 15) Documentation and Plans

- `README.md` — Project overview and getting-started.
- `ARCHITECTURE.md`, `DATAFLOW.md`, `PROFESSIONAL_ARCHITECTURE.md` — Architecture references.
- `VULKAN_PATH_PLAN.md`, `TODO_PRO_TRACKER.md`, `*_PLAN.md` — Execution plans and tracking.
- `DEBUG.md`, `TEST_SUMMARY.md`, `ENDPOINT_TESTING_SUMMARY.md` — Debug/testing notes.

Position: cross-cutting documentation.

## 16) Organizational Paradigm Summary

The effective architecture is:
1. `L0` binaries/entrypoints (`jcut`, headless tools).
2. `L1` orchestration (`editor_*`, manager/controllers).
3. `L2` domain/state/timing/serialization (`editor_shared*`, timeline/project models).
4. `L3` render/decode/media pipelines (render/decode/cache/backends/compositor).
5. `L4` UI presentation and interactions (preview/timeline/inspector/tabs).
6. `L5` integration surfaces and operations (control server, scripts, AI helpers, diagnostics).

Use this layering rule of thumb:
- `L4` should call into `L1/L2/L3`, not own codec/backend policy.
- `L3` should be backend-policy aware but UI-agnostic.
- `L2` should stay deterministic and side-effect light.
- `L5` can orchestrate any layer but should not become business-logic source-of-truth.
