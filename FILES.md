# FILES.md

This document describes the current first-party source, script, and documentation file groups at the repository root.

Scope:
- Includes top-level first-party files, scripts, tests, docs, and first-party subdirectories.
- Excludes vendored/generated dependency trees such as `external/`, `ffmpeg/`, `ffmpeg-build/`, `ffmpeg-install/`, `rtaudio/`, `nv-codec-headers/`, and build output when present.
- Uses grouped entries where files form one subsystem.

Layer labels:
- `L0` Entry points and binaries
- `L1` Application orchestration
- `L2` Domain, state, timing, and serialization
- `L3` Rendering, media, decode, audio, and GPU pipelines
- `L4` UI presentation and interactions
- `L5` Integration, tooling, diagnostics, docs, and tests

Ownership model:
- Each file group below has exactly one primary ownership section. A file can list multiple layer labels when it crosses a boundary, but it should not be repeated in another ownership group.
- The primary owner is responsible for construction/destruction, persistent state, thread affinity, resource lifetime, public API shape, and tests for that group.
- Cross-owner behavior should move through a narrow public interface owned by the callee subsystem. Do not share mutable implementation state across groups to avoid circular lifecycle and shutdown bugs.
- New code belongs in the most specific existing owner group. Create a new group only when the code introduces an independently owned lifecycle, resource, artifact format, or user-facing workflow.

Dependency guardrails:
- UI presentation and interaction code may call orchestration, domain, and media/render service interfaces, but it should not own codec, Vulkan, FFmpeg, audio-device, or cache policy.
- Media, render, decode, audio, and GPU code should not depend on Qt Widgets tabs or editor panes. When backend code needs UI data, pass immutable request/config/value objects from an owning orchestration layer.
- Domain, timing, serialization, assignment, and artifact-model code should stay deterministic and avoid owning runtime devices, threads, GPU handles, FFmpeg contexts, or Qt widgets.
- Entry points should parse process-level options and delegate. They should not accumulate reusable pipeline logic.
- Diagnostics, scripts, benchmark tools, and planning docs can observe or exercise the product, but they should not become the only source of production behavior.

API and implementation ownership:
- Public interfaces own stable contracts and should expose small value types, handles, and service calls rather than backend implementation details.
- Backend implementation files own backend-private helpers, caches, handles, and compatibility paths for their subsystem.
- If a helper is used by one backend family, keep it with that backend owner. Promote it to a shared owner only when at least two independent production subsystems use it and the dependency direction remains clean.
- Header files should describe interfaces, value types, lightweight inline operations, and templates. Non-trivial implementation belongs in source files unless there is a clear compile-time reason.

Runtime, artifact, and test ownership:
- Decode owns decode worker threads and packet/frame flow. Audio owns audio device callbacks, clock handoff, and audio-buffer lifetime. Vulkan/offscreen/render backends own their GPU objects and synchronization. UI owns GUI-thread-only widgets and user interaction state.
- Timeline cache owns decoded-frame cache residency. Memory budget owns memory pressure policy. Frame-handoff code owns cross-backend detector handoff resources. Face-detection offscreen code owns generated face-stream artifact writes, checkpoint/resume behavior, and benchmark output.
- Artifact readers and migration tools should preserve backward compatibility at the artifact boundary while keeping artifact schema decisions in the face-detection domain/runtime owner.
- Tests should live with the behavior they validate: backend contract tests for backend APIs, artifact/tracking tests for face-detection domain behavior, UI smoke tests for UI wiring, and CLI smoke tests for entry-point delegation.

## Build, Entry Points, and Project Scripts

- `CMakeLists.txt` — Main build graph, targets, feature probes, and dependency wiring. Position: `L0/L5`.
- `Makefile`, `build.sh`, `cleanup.sh`, `run_editor.sh`, `mac_dmg.sh` — Developer build/run/package helpers. Position: `L5`.
- `editor_main.cpp` — Main GUI process entry and command-line routing. Position: `L0`.
- `backend_headless_compare_main.cpp`, `project_roundtrip_cli.cpp`, `migrate_facedetections_artifacts_main.cpp`, `vulkan_zero_copy_video_preprocess_main.cpp`, `vulkan_facedetections_offscreen_main.cpp` — Standalone CLI/tool entry points. Position: `L0/L5`.
- `build_info.h.in`, `editor.config`, `ui_icons.qrc`, `qt_compat.h`, `ffmpeg_compat.h` — Build/config/resource compatibility files. Position: `L0/L5`.

## Editor Application Orchestration

- `editor.cpp`, `editor.h` — Main editor shell object lifecycle. Position: `L1`.
- `editor_setup.cpp`, `startup_project_state.cpp`, `startup_project_state.h` — Startup wiring and initial project state. Position: `L1`.
- `editor_editor_pane.cpp`, `editor_pane.cpp`, `editor_pane.h` — Editor pane composition and shared pane container behavior. Position: `L1/L4`.
- `editor_tabs.cpp`, `editor_input.cpp`, `editor_inspector_bindings.cpp` — Tab, input, and inspector binding orchestration. Position: `L1/L4`.
- `editor_playback.cpp`, `editor_playback_types.h`, `playback_controller.cpp`, `playback_controller.h`, `playback_clock_coordinator.cpp`, `playback_clock_coordinator.h`, `playback_frame_pipeline.cpp`, `playback_frame_pipeline.h`, `playback_debug.h`, `playback_stage_metrics.h` — Playback orchestration, timing, and stage diagnostics. Position: `L1/L3`.
- `editor_media_tools.cpp`, `editor_render_tools.cpp`, `editor_profiling.cpp`, `editor_optimization.cpp` — Editor actions for media, render/export, profiling, and optimization. Position: `L1/L3/L5`.
- `editor_preview_edit_helpers.cpp`, `editor_preview_edit_helpers.h` — Shared preview edit helper logic. Position: `L1/L4`.

## Domain, Project, Timeline, and Serialization

- `editor_shared.cpp`, `editor_shared.h`, `editor_shared_core.h`, `editor_shared_effects.cpp`, `editor_shared_effects.h`, `editor_shared_keyframes.cpp`, `editor_shared_keyframes.h`, `editor_shared_keyframes_cache.cpp`, `editor_shared_keyframes_cache.h`, `editor_shared_media.cpp`, `editor_shared_media.h`, `editor_shared_render_sync.cpp`, `editor_shared_render_sync.h`, `editor_shared_timing.cpp`, `editor_shared_timing.h`, `editor_shared_transcript.cpp`, `editor_shared_transcript.h`, `editor_shared_transcript_overlay.cpp` — Canonical shared editor domain data and helpers. Position: `L2`.
- `clip_serialization.cpp`, `clip_serialization.h` — Timeline clip JSON serialization/deserialization. Position: `L2`.
- `project_state.cpp`, `project_manager.cpp`, `project_manager.h`, `projects.cpp`, `projects.h` — Project state, discovery, open/save coordination. Position: `L1/L2`.
- `timeline_container.cpp`, `timeline_container.h`, `timeline_layout.cpp`, `timeline_layout.h`, `timeline_renderer.cpp`, `timeline_renderer.h`, `timeline_fps.h` — Timeline model/container geometry and drawing support. Position: `L2/L4`.
- `timeline_widget.h`, `timeline_widget_core.cpp`, `timeline_widget_input.cpp`, `timeline_widget_layout.cpp`, `timeline_widget_model.cpp`, `timeline_widget_paint.cpp`, `timeline_widget_context_menu.cpp` — Timeline widget split by interaction/render concern. Position: `L4`.
- `animation.cpp`, `animation.h`, `model.cpp`, `model.h`, `model_runtime.cpp`, `model_import_shared.cpp`, `model_import_shared.h`, `gltf_import.cpp`, `gltf_import.h`, `fbx_import.cpp`, `fbx_import.h` — Model/import/runtime helpers. Position: `L2/L3`.

## Decode, Cache, Frame, and Memory Pipeline

- `decoder_context.cpp`, `decoder_context.h` — Decode context lifecycle and packet/frame flow. Position: `L3`.
- `decoder_ffmpeg_utils.cpp`, `decoder_ffmpeg_utils.h`, `decoder_image_io.cpp`, `decoder_image_io.h`, `decoder_benchmark_utils.cpp`, `decoder_benchmark_utils.h` — Decode helpers, image loading, and benchmark support. Position: `L3/L5`.
- `async_decoder.cpp`, `async_decoder.h`, `decode_trace.cpp`, `decode_trace.h` — Background decode and instrumentation. Position: `L3/L5`.
- `frame_handle.cpp`, `frame_handle.h`, `frame_buffer_utils.h`, `media_pipeline_shared.cpp`, `media_pipeline_shared.h` — CPU/GPU frame ownership and shared media pipeline primitives. Position: `L3`.
- `timeline_cache.cpp`, `timeline_cache.h`, `timeline_cache_requests.cpp`, `timeline_cache_storage.cpp`, `timeline_cache_seek_resync.cpp`, `timeline_cache_seek_resync.h` — Timeline frame cache request, storage, and seek-resync behavior. Position: `L3`.
- `memory_budget.cpp`, `memory_budget.h` — Memory pressure accounting and policy. Position: `L3/L5`.

## Audio Runtime

- `audio_engine.cpp`, `audio_engine.h` — Audio playback engine implementation and interface. Position: `L3`.
- `audio_preview_support.cpp`, `audio_preview_support.h` — Audio preview helper path. Position: `L3/L4`.
- `audio_time_stretch.cpp`, `audio_time_stretch.h`, `audio_time_stretch_cache.cpp`, `audio_time_stretch_cache.h` — Time-stretch engine and cache. Position: `L3`.
- `waveform_service.cpp`, `waveform_service.h` — Waveform generation/cache service. Position: `L3`.
- `vulkan_audio_tab.cpp`, `vulkan_audio_tab.h` — Vulkan audio diagnostics UI. Position: `L4/L5`.

## Export and Render Pipeline

- `render.h`, `render_internal.h`, `render.md` — Public and internal render contracts plus notes. Position: `L3/L5`.
- `render_backend.cpp`, `render_backend.h` — Backend selection and policy parsing. Position: `L2/L3`.
- `render_export.cpp`, `render_decode.cpp`, `render_audio.cpp`, `render_codecs.cpp`, `render_gpu.cpp`, `render_stats.cpp` — Export orchestration, decode/audio/codec/GPU paths, and render metrics. Position: `L3`.
- `cpu_render_fallback.cpp`, `cpu_render_fallback.h`, `cpu_overlay_render_backend.cpp`, `cpu_overlay_render_backend.h` — CPU render/color-conversion and overlay fallback utilities. Position: `L3`.
- `speaker_export_harness.cpp`, `speaker_export_harness.h` — Headless speaker/export harness. Position: `L0/L3/L5`.

## Preview Surface and Renderer Backend Abstractions

- `preview_surface.h`, `preview_surface_factory.cpp`, `preview_surface_factory.h`, `preview_widget_factory.cpp`, `preview_widget_factory.h`, `null_preview_surface.cpp`, `null_preview_surface.h` — Preview surface abstraction and backend-aware construction. Position: `L3/L4`.
- `preview_renderer_backend.cpp`, `preview_renderer_backend.h` — QRhi preview renderer backend initialization. Position: `L3`.
- `preview_view_transform.cpp`, `preview_view_transform.h`, `preview_frame_selection.cpp`, `preview_frame_selection.h`, `preview_interaction_state.h`, `preview_overlay_model.h` — Preview geometry, selection, interaction, and overlay model helpers. Position: `L2/L4`.
- `preview_face_track_click_policy.cpp`, `preview_face_track_click_policy.h`, `preview_speaker_profiles.cpp`, `preview_speaker_profiles.h` — Preview face/track click behavior and speaker profile helpers. Position: `L2/L4`.
- `facestream_overlay_snapshot.cpp`, `facestream_overlay_snapshot.h`, `editor_facedetections_preview_harness.cpp` — Face-stream overlay snapshot and preview harness support. Position: `L4/L5`.

## OpenGL Preview Path

- `opengl_preview.h` — OpenGL preview widget contract/state. Position: `L4/L3`.
- `opengl_preview_window_core.cpp`, `opengl_preview_window_gl.cpp`, `opengl_preview_window_pipeline.cpp`, `opengl_preview_window_overlay.cpp`, `opengl_preview_window_overlay_support.cpp`, `opengl_preview_window_transcript.cpp`, `opengl_preview_window_transcript_gpu.cpp`, `opengl_preview_window_interaction.cpp` — OpenGL preview lifecycle, GL drawing, pipeline glue, overlays, transcript, and input. Position: `L4/L3`.
- `opengl_preview_debug.cpp`, `opengl_preview_debug.h` — Preview debug probes and diagnostics. Position: `L5`.
- `gl_frame_texture_shared.cpp`, `gl_frame_texture_shared.h` — Shared GL texture helpers. Position: `L3`.

## Vulkan Preview, Render, and GPU Infrastructure

- `vulkan_backend.cpp`, `vulkan_backend.h`, `vulkan_pipeline.cpp`, `vulkan_pipeline.h`, `vulkan_resources.cpp`, `vulkan_resources.h`, `vulkan_clear_helpers.cpp`, `vulkan_clear_helpers.h` — Vulkan backend, pipeline/resource, and clear helpers. Position: `L3`.
- `vulkan_preview_surface.cpp`, `vulkan_preview_surface.h`, `vulkan_preview_surface_facedetections.cpp`, `vulkan_preview_surface_profiling.cpp` — Vulkan preview surface and profiling/face-detection hooks. Position: `L3/L4`.
- `direct_vulkan_preview_backend.h` — Direct Vulkan preview backend interface. Position: `L3/L4`.
- `direct_vulkan_preview_presenter.cpp`, `direct_vulkan_preview_presenter.h`, `direct_vulkan_preview_window.cpp`, `direct_vulkan_preview_host_widget.cpp`, `direct_vulkan_preview_audio.cpp`, `direct_vulkan_preview_audio.h`, `direct_vulkan_preview_config.cpp`, `direct_vulkan_preview_config.h`, `direct_vulkan_preview_geometry.cpp`, `direct_vulkan_preview_geometry.h`, `direct_vulkan_preview_interaction.cpp`, `direct_vulkan_preview_interaction.h`, `direct_vulkan_preview_overlay_rendering.cpp`, `direct_vulkan_preview_overlay_rendering.h` — Direct `QVulkanWindow` preview presenter, host widget, audio, configuration, geometry, interaction, and overlay rendering. Position: `L3/L4`.
- `direct_vulkan_frame_handoff_pipeline.cpp`, `direct_vulkan_frame_handoff_pipeline.h`, `vulkan_detector_frame_handoff.cpp`, `vulkan_detector_frame_handoff.h` — Vulkan frame handoff and detector interop pipeline. Position: `L3`.
- `offscreen_vulkan_renderer.cpp`, `offscreen_vulkan_renderer_backend.cpp`, `offscreen_vulkan_renderer_backend.h`, `offscreen_vulkan_renderer_helpers.cpp`, `offscreen_vulkan_renderer_helpers.h` — Offscreen Vulkan renderer public API, backend implementation, and helpers. Position: `L3`.
- `gpu_compositor.cpp`, `gpu_compositor.h`, `visual_effects_shader.h`, `polygon_triangulation.h`, `vulkan_text_renderer.cpp`, `vulkan_text_renderer.h` — GPU compositing, shader helpers, polygon triangulation, and Vulkan text rendering. Position: `L3`.
- `shaders/vulkan/` — First-party Vulkan shader sources. Position: `L3`.

## Face Detection, Face Streams, and Assignment

- `facedetections_types.h`, `facedetections_generation.cpp`, `facedetections_generation.h`, `facedetections_runtime.cpp`, `facedetections_runtime.h`, `facedetections_tracking.cpp`, `facedetections_tracking.h`, `facedetections_time_mapping.cpp`, `facedetections_time_mapping.h`, `facedetections_artifact_utils.h`, `facedetections_continuity_artifacts.cpp`, `facedetections_assignment_services.cpp`, `facedetections_assignment_services.h` — Face-detection domain types, generation helpers, runtime, tracking, artifact, mapping, and assignment services. Position: `L2/L3`.
- `detector_settings.cpp`, `detector_settings.h`, `facefind_window.cpp`, `facefind_window.h` — Detector settings UI/runtime and face-find window. Position: `L2/L4`.
- `vulkan_zero_copy_face_detector.cpp`, `vulkan_zero_copy_face_detector.h`, `vulkan_res10_ncnn_face_detector.cpp`, `vulkan_res10_ncnn_face_detector.h`, `vulkan_scrfd_ncnn_face_detector.cpp`, `vulkan_scrfd_ncnn_face_detector.h` — Vulkan/ncnn detector implementations. Position: `L3`.
- `vulkan_facedetections_offscreen_runner.cpp`, `vulkan_facedetections_offscreen_runner.h` — Offscreen face-detection top-level run orchestration. Position: `L1/L3`.
- `vulkan_facedetections_offscreen_options.cpp`, `vulkan_facedetections_offscreen_options.h`, `vulkan_facedetections_offscreen_tuning.cpp`, `vulkan_facedetections_offscreen_tuning.h`, `vulkan_facedetections_offscreen_control_panel.cpp`, `vulkan_facedetections_offscreen_control_panel.h`, `vulkan_facedetections_offscreen_progress.cpp`, `vulkan_facedetections_offscreen_progress.h` — Offscreen CLI options, runtime tuning, control panel, and progress UI helpers. Position: `L1/L4/L5`.
- `vulkan_facedetections_offscreen_detection_filters.cpp`, `vulkan_facedetections_offscreen_detection_filters.h`, `vulkan_facedetections_offscreen_vulkan_harness.cpp`, `vulkan_facedetections_offscreen_vulkan_harness.h`, `vulkan_facedetections_offscreen_opencv_dnn.cpp`, `vulkan_facedetections_offscreen_opencv_dnn.h` — Detection filtering plus Vulkan/OpenCV detector harness paths. Position: `L3`.
- `vulkan_facedetections_offscreen_artifact_io.cpp`, `vulkan_facedetections_offscreen_artifact_io.h`, `vulkan_facedetections_offscreen_preview_io.cpp`, `vulkan_facedetections_offscreen_preview_io.h`, `vulkan_facedetections_offscreen_checkpoint_writer.cpp`, `vulkan_facedetections_offscreen_checkpoint_writer.h`, `vulkan_facedetections_offscreen_resume_state.cpp`, `vulkan_facedetections_offscreen_resume_state.h`, `vulkan_facedetections_offscreen_benchmark.cpp`, `vulkan_facedetections_offscreen_benchmark.h` — Offscreen artifact IO, preview IO, checkpoint writing, resume state, and benchmark support. Position: `L3/L5`.
- `scripts/migrate_facedetections_artifacts.sh`, `face_tracer_bench.py` — Face-detection migration and benchmark tooling. Position: `L5`.

## UI Tabs and Panels

- `inspector_pane.cpp`, `inspector_pane.h`, `inspector_pane_collections.cpp`, `inspector_pane_secondary_tabs.cpp`, `inspector_pane_wiring.cpp`, `inspector_controller.cpp`, `inspector_controller.h` — Inspector shell, tab collections, secondary tabs, wiring, and controller. Position: `L4/L1`.
- `explorer_pane.cpp`, `explorer_pane.h`, `output_tab.cpp`, `output_tab.h`, `pipeline_tab.cpp`, `pipeline_tab.h`, `profile_tab.cpp`, `profile_tab.h`, `history_tab.cpp`, `history_tab.h`, `properties_tab.cpp`, `properties_tab.h` — Explorer, output, pipeline, profile, history, and properties UI panels. Position: `L4`.
- `clips_tab.cpp`, `clips_tab.h`, `tracks_tab.cpp`, `tracks_tab.h`, `track_sidebar.cpp`, `track_sidebar.h`, `tracks.cpp` — Clip/track UI and sidebars. Position: `L4`.
- `effects_tab.cpp`, `effects_tab.h`, `opacity_tab.cpp`, `opacity_tab.h`, `grading_tab.cpp`, `grading_tab.h`, `grading_tab_curve.cpp`, `grading_histogram_widget.cpp`, `grading_histogram_widget.h`, `corrections_tab.cpp`, `corrections_tab.h`, `video_keyframe_tab.cpp`, `video_keyframe_tab.h`, `keyframe_tab_base.cpp`, `keyframe_tab_base.h`, `keyframe_table_shared.cpp`, `keyframe_table_shared.h`, `table_tab_base.cpp`, `table_tab_base.h`, `editor_tab_edit_effects.h` — Visual effects, grading, correction, and keyframe editing UI. Position: `L4`.
- `titles.cpp`, `titles.h`, `titles_tab.cpp`, `titles_tab.h`, `transport_icons.cpp`, `transport_icons.h`, `assets/icons/transport/` — Titles and transport icon assets. Position: `L2/L4`.

## Transcript, Sync, and Speaker Workflows

- `transcript_engine.cpp`, `transcript_engine.h`, `transcript_runtime_cache.cpp`, `transcript_runtime_cache.h`, `transcript_tab.cpp`, `transcript_tab.h`, `transcript_tab_document.cpp`, `transcript_document_edit_service.cpp`, `transcript_document_edit_service.h`, `transcript_document_io.cpp`, `transcript_document_io.h`, `transcript_document_save_controller.cpp`, `transcript_document_save_controller.h`, `transcript_document_session.cpp`, `transcript_document_session.h` — Transcript parsing, runtime cache, UI, document IO/edit/save/session services. Position: `L2/L4`.
- `transform_skip_aware_timing.cpp`, `transform_skip_aware_timing.h`, `sync_tab.cpp`, `sync_tab.h`, `sync_detector.py` — Sync and skipped-region timing logic. Position: `L2/L4/L5`.
- `speakers_tab.cpp`, `speakers_tab.h`, `speakers_tab_facedetections_actions.cpp`, `speakers_tab_facedetections_engines.cpp`, `speakers_tab_facedetections_generation.cpp`, `speakers_tab_interactions.cpp`, `speakers_tab_interactions_ai.cpp`, `speakers_tab_interactions_ai_mining.cpp`, `speakers_tab_internal.h`, `speakers_tab_navigation.cpp`, `speakers_tab_reference_preview.cpp`, `speakers_tab_wiring.cpp`, `speakers_table.cpp`, `speakers_table.h` — Speaker UI, face-detection integration, AI interactions, navigation, and table support. Position: `L4/L1`.
- `speaker_document_edit_ops.cpp`, `speaker_document_edit_ops.h`, `speaker_flow_debug.cpp`, `speaker_flow_debug.h`, `speaker_section_selection_timing_service.cpp`, `speaker_section_selection_timing_service.h`, `speaker_track_assignment_service.cpp`, `speaker_track_assignment_service.h`, `identity_resolution.cpp`, `identity_resolution.h`, `track_avatar_utils.cpp`, `track_avatar_utils.h` — Speaker document operations, debug, timing, track assignment, identity, and avatar utilities. Position: `L2/L4`.

## Control Server, AI, and Integration

- `control_server.cpp`, `control_server.h`, `control_server_worker.cpp`, `control_server_worker.h`, `control_server_worker_routes.cpp`, `control_server_worker_routes_ui.cpp`, `control_server_http_utils.cpp`, `control_server_http_utils.h`, `control_server_ui_utils.cpp`, `control_server_ui_utils.h`, `control_server_media_diag.cpp`, `control_server_media_diag.h`, `control_server_webpage.html` — Local control server, routes, HTTP helpers, UI state helpers, media diagnostics, and built-in web page. Position: `L5`.
- `editor_ai_integration.cpp`, `editor_ai_helpers.cpp`, `editor_ai_helpers.h`, `ai_rest_snapshot.py` — AI integration and REST snapshot tooling. Position: `L1/L5`.
- `legacy/` — Legacy first-party face/speaker helper scripts and Dockerfile retained outside the active top-level source path. Position: `L5`.
- `loiacono/` — First-party audio/spectrum experimental app and tests. Position: `L0/L3/L4/L5`.

## Tests, Harnesses, and Diagnostics

- `tests/` and `tests/CMakeLists.txt` — Unit/integration tests for playback, rendering, Vulkan, transcript, face-detection, waveform, and UI contracts. Position: `L5`.
- `test.cpp`, `test2.cpp`, `test3.cpp`, `test4.cpp`, `test5.cpp`, `test6.cpp`, `test7.cpp`, `test8.cpp`, `test9.cpp`, `test10.cpp`, `test11.cpp`, `test12.cpp`, `test13.cpp` — Ad hoc/legacy top-level test programs. Position: `L5`.
- `test_connection.sh`, `test_webpage.sh`, `tests/ai_integration_smoke.sh`, `tests/rest_smoke_test.py`, `tests/ui_smoke_harness.py`, `tests/live_playback_lag_probe.py` — Smoke/probe scripts. Position: `L5`.
- `scripts/capture_callstacks.sh`, `scripts/scan_redundant_code.sh`, `scripts/vulkan_headless_export_compare.sh`, `scripts/vulkan_parity_throughput.sh`, `scripts/vulkan_parity_throughput_headless.sh` — Diagnostics and benchmark scripts. Position: `L5`.
- `debug_controls.cpp`, `debug_controls.h`, `callstackcapture.md`, `scheduling.md`, `synchronization.md`, `skill_local_curl.md`, `migration.md` — Debug controls and operational notes. Position: `L5`.

## Documentation and Planning

- `README.md`, `ARCHITECTURE.md`, `DATAFLOW.md`, `PROFESSIONAL_ARCHITECTURE.md`, `STARTUP_SEQUENCE.md`, `TRANSFORMATIONS_PATHWAY.md`, `TRANSCRIPT_LOGIC.md`, `SPEAKER_FLOW.md`, `UI_LAYOUT.md`, `TABS.md`, `FEATURES.md`, `AUDIO_PREVIEW_AND_DYNAMICS.md` — Project documentation. Position: `L5`.
- `AI_INTEGRATION.md`, `DEBUG.md`, `ENDPOINT_TESTING_SUMMARY.md`, `TEST_SUMMARY.md`, `TIME.md`, `TODO.md`, `todo.md`, `REMAINING_WORK.md`, `OPTIMIZE.md`, `PROBLEMS.md`, `PROFESSIONALIZE.md` — Planning, status, and operations notes. Position: `L5`.
- `DIRECT_VULKAN_PREVIEW_BREAKDOWN_PLAN.md`, `FILESIZE_REDUCTION_STRATEGY.md`, `OPENGL_DEPRECATION_AND_REMOVAL_PLAN.md`, `QT_DEPRECATION_PLAN.md`, `REFACTOR_PLAN.md`, `UX_AI_AUDIO_FIXES_PLAN.md`, `WAVEFORM_IMPROVEMENT_PLAN.md`, `detection_speed_improvement_plan.md`, `directvulcanmodularization.md`, `vulkan_facedetections_offscreen_modularization.md`, `model_refactor_notes.txt`, `INTEGRATION_NOTES.txt` — Active and historical planning documents. Position: `L5`.
- `docs/` — First-party diagrams and ncnn fork notes. Position: `L5`.

## Other Root-Level Artifacts

- `editor_state.json.bak`, `testbench_state.json`, `DartConfiguration.tcl`, `CTestTestfile.cmake`, `cmake_install.cmake` — Local/test/config artifacts currently present at the repository root. Position: `L5`.
- `render_audio.cpp.backup`, `professional_example.cpp`, `profile_editor.py`, `restore_skipped_transcript_words.py`, `whisperx.sh`, `sam3.sh`, `jasonremove.md`, `plan.md` — Miscellaneous local utilities, examples, backups, and notes currently present at the repository root. Position: `L5`.
- Large local data/profiling files such as `nasreen.mp4`, `perf.data`, `perf.data.old`, `heaptrack.jcut.43571.zst`, and `hftoken.txt` are present in this workspace but are not source modules.

## Organizational Rule of Thumb

1. `L0` owns binaries and process entry.
2. `L1` wires user workflows and application lifecycle.
3. `L2` owns deterministic domain/state/timing logic.
4. `L3` owns media, audio, render, decode, and GPU pipelines.
5. `L4` owns UI presentation and interaction.
6. `L5` owns integration surfaces, diagnostics, scripts, tests, and documentation.

Prefer dependencies flowing from UI/orchestration into domain/media services. Avoid moving codec/backend policy into UI classes, and avoid turning diagnostic scripts into source-of-truth business logic.
