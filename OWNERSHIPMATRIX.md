# Refactor Ownership Matrix

## Purpose

This matrix is the source-of-truth for deciding where code from the large-file
refactor belongs. It prevents a file split from creating a second owner for an
existing responsibility.

Decision meanings:

- **Keep**: the current file is already the correct owner.
- **Move to existing**: migrate into an existing module; do not create a new
  destination.
- **Consolidate**: multiple implementations exist; establish one canonical
  implementation and migrate callers after characterization tests.
- **Create**: no existing file has the responsibility; create the named module.
- **Delegate**: orchestration stays local while implementation remains in an
  existing service.

## Mandatory Rules

1. Search the repository for the responsibility and its key symbols before
   creating any file listed as **Create**.
2. A similarly named helper is not automatically equivalent. Add tests for
   edge cases, normalize signatures, and only then remove duplicates.
3. UI classes may orchestrate services but must not acquire a second copy of
   persistence, artifact parsing, decoding, rendering, or tracking logic.
4. Shared helpers belong in the narrowest domain module that has at least two
   real callers. Do not create generic `utils` files.
5. Existing modules remain authoritative unless this document explicitly
   replaces their ownership.
6. Use `./build.sh` from the repository root for all builds and build
   verification. Do not invoke CMake, Ninja, Make, or other underlying build
   tools directly.

## Speaker, Section, and Track Domain

| Responsibility / symbols | Current location(s) | Canonical owner | Decision | Notes |
|---|---|---|---|---|
| Transcript document load/save/queued flush | `speakers_tab.cpp`, `transcript_tab_document.cpp`, transcript document services | `transcript_document_edit_service.*` and document I/O modules | Delegate | `SpeakersTab` keeps request/state orchestration only. Do not create another persistence implementation in `speakers_tab_document.cpp`. |
| Loaded speaker-document UI state | `speakers_tab.cpp` | `speakers_tab_document.cpp` | Create | Own load-request lifecycle and application of a loaded snapshot to speaker UI. It calls document services for I/O. |
| Speaker table presentation | `speakers_tab.cpp`, `speakers_table.cpp` | `speakers_table.*` | Move to existing | Extend the existing table module for row presentation. Keep selection coordination in `speakers_tab_navigation.cpp`. |
| Speaker selection and navigation | `speakers_tab.cpp`, `speakers_tab_navigation.cpp` | `speakers_tab_navigation.cpp` | Move to existing | Includes row lookup, select-by-id, and playhead-driven navigation. |
| Speaker-section table model and refresh | `speakers_tab.cpp` | `speakers_tab_sections_table.cpp` | Create | Own row construction and incremental refresh, not assignment mutation or persistence. |
| Track avatar decoding and representative frames | `tracks.cpp`, `speakers_tab_reference_preview.cpp`, `track_avatar_utils.cpp` | `track_avatar_utils.*` for reusable image selection; `speakers_tab_reference_preview.cpp` for speaker UI | Consolidate | Move reusable decode/frame selection to `track_avatar_utils`; keep tooltip/widget behavior in the speaker reference-preview module. Do not create `speakers_tab_track_avatars.cpp` until both existing owners are evaluated. |
| Face-detection generation UI actions | `speakers_tab.cpp`, `speakers_tab_facedetections_actions.cpp`, `speakers_tab_facedetections_generation.cpp` | `speakers_tab_facedetections_actions.cpp` | Move to existing | Actions invoke generation owned by `speakers_tab_facedetections_generation.cpp`; engines remain in `speakers_tab_facedetections_engines.cpp`. |
| Generation implementation and process lifecycle | `speakers_tab.cpp`, `speakers_tab_facedetections_generation.cpp` | `speakers_tab_facedetections_generation.cpp` | Consolidate | Status polling/process details move here; the tab only presents status. |
| Tracking/generator status presentation | `speakers_tab.cpp` | `speakers_tab_status.cpp` | Create | UI labels only. Process inspection and generation state remain with generation/runtime owners. |
| Assignment resolution | `tracks.cpp`, `facedetections_assignment_services.cpp`, `identity_resolution.cpp` | `facedetections_assignment_services.*` | Consolidate | Move pure resolution logic into the service. `SpeakersTab` may cache and render results but must not reimplement resolution rules. |
| Assignment persistence and document mutations | `tracks.cpp`, `speaker_track_assignment_service.cpp`, `speaker_document_edit_ops.cpp` | `speaker_track_assignment_service.*` and `speaker_document_edit_ops.*` | Move to existing | UI actions call these services. Do not put JSON mutation rules in new UI files. |
| Contiguous-section assignment commands | `tracks.cpp` | `speaker_track_assignment_service.*` | Consolidate | Extend the service with section-scoped operations; keep prompts/selection in `speakers_tab_track_actions.cpp`. |
| Track assignment user actions and context menus | `tracks.cpp`, `speakers_tab_interactions.cpp` | `speakers_tab_track_actions.cpp` | Create | UI command orchestration only; delegates mutations to assignment services. |
| Track candidate list and picker dialogs | `tracks.cpp` | `speakers_tab_track_picker.cpp` | Create | Own list population, picker interaction, and choice dispatch; no assignment persistence. |
| Face-box click policy | `tracks.cpp`, `preview_face_track_click_policy.cpp` | `preview_face_track_click_policy.*` | Move to existing | Policy/decision logic belongs in the existing policy module; speaker UI performs the selected action. |
| Face-detection paths/raw diagnostic panels | `tracks.cpp`, `facedetections_runtime.cpp`, artifact modules | `speakers_tab_track_diagnostics.cpp` for UI; existing runtime/artifact modules for data | Create + Delegate | New file owns panel presentation only. It must consume snapshots rather than parse artifacts independently. |
| Tracking/runtime caches | `tracks.cpp`, `facedetections_runtime.cpp`, `editor_shared_keyframes_cache.cpp` | Existing runtime/cache module matching the cache's consumer | Consolidate | Cache ownership follows data lifetime, not the UI displaying it. Document each cache before moving it. |

### Known Speaker-Domain Duplicate Helpers

| Duplicate family | Current copies | Canonical owner | Required action |
|---|---|---|---|
| Section track-entry parsing and ID extraction | `sectionTrackEntries*` / `sectionTrackIdStrings*` in `tracks.cpp`, `speakers_tab.cpp`, `editor_shared_keyframes.cpp` | `speaker_section_assignment_utils.*` | Create a narrow domain module, characterize legacy/single-ID and array formats, migrate all three callers, then delete copies. |
| Transcript seconds-to-frame conversion | `sectionTranscriptFrameForSeconds*` in `tracks.cpp` and `editor_shared_keyframes.cpp` | Existing transcript timing utility if suitable; otherwise `speaker_section_assignment_utils.*` | Consolidate after testing rounding, negative values, and transcript FPS assumptions. |
| Clip display labels | `clipDisplayLabel*` in `tracks.cpp` and `speakers_tab.cpp` | Existing media/clip presentation helper if present; otherwise `speakers_tab_reference_preview.cpp` | Consolidate only if both UI contexts require identical fallback behavior. |
| Local-to-source preview frame mapping | `clipSourceFrameForLocal*` in `tracks.cpp` and `speakers_tab.cpp` | Shared media timing module (`editor_shared_timing.*`) | Move to existing after verifying trim, speed, reverse, and bounds behavior. |
| JSON object-key diagnostics | `objectKeysJson` in `tracks.cpp` and `speakers_tab.cpp` | Local diagnostic snapshot code | Keep local or remove | Too trivial to justify a shared utility. Prefer deleting it if diagnostics can use an existing JSON helper. |
| Speaker-section key construction | `speakerSectionKey` and related key formats | `speaker_section_assignment_utils.*` | Consolidate | One serialized key format and one parser/formatter pair. Add compatibility tests before migration. |

## Audio Domain

| Responsibility | Current location(s) | Canonical owner | Decision | Boundary |
|---|---|---|---|---|
| Device initialization, shutdown, callback, transport, clock, volume | `audio_engine.cpp` | `audio_engine_device.cpp` | Create | Device and real-time callback lifecycle only. |
| Timeline/source scheduling and readiness | `audio_engine.cpp`, `audio_mix_readiness.h` | `audio_engine_schedule.cpp` plus `audio_mix_readiness.h` for pure readiness types/helpers | Create + Consolidate | Scheduling owns queues and mapping; readiness header must not acquire engine state. |
| Source audio decoding and decode worker | `audio_engine.cpp` | `audio_engine_decode.cpp` | Create | Decode and source-cache population only. |
| Time-stretch algorithm | `audio_engine.cpp`, `audio_time_stretch.cpp` | `audio_time_stretch.*` | Move to existing | Algorithm and Rubber Band transformation belong here. |
| Time-stretch cache/sidecars/single-flight loads | `audio_engine.cpp`, `audio_time_stretch_cache.cpp` | `audio_time_stretch_cache.*` | Move to existing | Extend the cache API instead of creating a parallel engine cache implementation. |
| Time-stretch job scheduling/progress | `audio_engine.cpp` | `audio_engine_time_stretch_jobs.cpp` | Create | Engine-specific queue and progress orchestration; delegates transform/cache work. |
| Speech isolation/harmonic processing | `audio_engine.cpp`, `audio_speech_harmonic_isolator.cpp` | `audio_speech_harmonic_isolator.*` | Move to existing | Mixing consumes the processed result. |
| Playback mixing, speech ranges, crossfades, dynamics, mix worker | `audio_engine.cpp` | `audio_engine_mix.cpp` | Create | Real-time-safe mixing only; no decoding or sidecar I/O. |
| Preview audio adaptation | `audio_preview_support.cpp`, `direct_vulkan_preview_audio.cpp` | Existing respective modules | Keep | Do not migrate into the engine core unless the engine API lacks a general primitive. |
| Export audio rendering | `render_audio.cpp` | `render_audio.cpp` | Keep | Export is a consumer of shared audio primitives, not an owner of playback engine state. |
| Waveform generation/cache | `waveform_service.cpp` | `waveform_service.*` | Keep | Never move into `AudioEngine`. |
| Ring buffer | `audio_engine.cpp` | `audio_engine_device.cpp` private implementation | Move | Create `audio_ring_buffer.*` only if a second production caller appears. |

## Offscreen Vulkan Renderer

| Responsibility | Current location(s) | Canonical owner | Decision | Boundary |
|---|---|---|---|---|
| Generic shared Vulkan resource abstractions | `vulkan_resources.cpp` | `vulkan_resources.*` | Move to existing | Reusable buffer/image/memory primitives only; no offscreen render policy. |
| Render/export Vulkan shared logic | `render_vulkan_shared.cpp` | `render_vulkan_shared.*` | Move to existing | Logic shared by more than one render backend. |
| Backend-independent calculations and conversions | `offscreen_vulkan_renderer_helpers.cpp`, backend file | `offscreen_vulkan_renderer_helpers.*` | Consolidate | Keep helpers pure and free of resource ownership. |
| Offscreen device/queue/command-pool lifecycle | `offscreen_vulkan_renderer_backend.cpp` | `offscreen_vulkan_renderer_device.cpp` | Create | Backend-specific lifetime and capability selection. |
| Offscreen descriptors, pipelines, images, buffers, and caches | backend file, `vulkan_resources.cpp` | `offscreen_vulkan_renderer_resources.cpp` using `vulkan_resources.*` | Create + Delegate | This module owns composition-specific resources but uses shared primitives. |
| Layer composition, masks, grading, overlays, draw submission | backend file | `offscreen_vulkan_renderer_composite.cpp` | Create | High-level GPU composition; text rendering remains in `vulkan_text_renderer.*`. |
| Detector frame transfer/handoff | backend file, `vulkan_detector_frame_handoff.cpp` | `vulkan_detector_frame_handoff.*` | Move to existing | Do not create a renderer-specific copy of synchronization or transfer logic. |
| BGRA/YUV staging and CPU readback | backend file | `offscreen_vulkan_renderer_readback.cpp` | Create | Backend-specific readback orchestration; generic conversions may move to helpers. |
| NV12 conversion and CUDA external-memory transfer | backend file | `offscreen_vulkan_renderer_nv12.cpp` | Create | Capability checks and transfer lifecycle stay together. |
| Public `OffscreenVulkanRenderer` facade and render orchestration | backend file and public headers | `offscreen_vulkan_renderer_backend.cpp` | Keep | Thin API forwarding and top-level `renderFrame` sequence only. |

## Direct Vulkan Preview

| Responsibility | Current location(s) | Canonical owner | Decision |
|---|---|---|---|
| Audio | `direct_vulkan_preview_audio.cpp`, window file | `direct_vulkan_preview_audio.*` | Move to existing |
| Geometry/view transforms | `direct_vulkan_preview_geometry.cpp`, window file | `direct_vulkan_preview_geometry.*` | Move to existing |
| Input and face-box interaction | `direct_vulkan_preview_interaction.cpp`, window file | `direct_vulkan_preview_interaction.*` | Move to existing |
| Presentation timing/state | `direct_vulkan_preview_presenter.cpp`, window file | `direct_vulkan_preview_presenter.*` | Move to existing |
| Transcript overlay data | `direct_vulkan_preview_transcript.cpp` | `direct_vulkan_preview_transcript.*` | Keep |
| Overlay drawing | `direct_vulkan_preview_overlay_rendering.cpp` | `direct_vulkan_preview_overlay_rendering.*` | Keep |
| Renderer/window private declarations | `direct_vulkan_preview_window.cpp` | `direct_vulkan_preview_internal.h` | Create |
| Renderer lifecycle and device loss | window file | `direct_vulkan_preview_lifecycle.cpp` | Create |
| Clip/title GPU resource caches | window file | `direct_vulkan_preview_resources.cpp` | Create |
| Swapchain and decoder readback | window file | `direct_vulkan_preview_readback.cpp` | Create |
| Per-frame composition/submission | window file | `direct_vulkan_preview_frame.cpp` | Create |
| Public window facade functions | window file | `direct_vulkan_preview_window.cpp` | Keep | 

## Vulkan Text, Titles, and Overlay Styling

This section incorporates the ownership seams added by commit `073eb8b`.

| Responsibility | Current location(s) | Canonical owner | Decision |
|---|---|---|---|
| Shared title/transcript text-style representation and conversion | `overlay_text_style.cpp` | `overlay_text_style.*` | Keep |
| Style serialization and backward-compatible defaults | `clip_serialization.cpp`, `overlay_text_style.cpp` | `clip_serialization.cpp` using `overlay_text_style.*` | Keep + Delegate |
| Transcript overlay cache-key policy | `transcript_overlay_cache_key.cpp`, renderer-local keys | `transcript_overlay_cache_key.*` for public transcript policy | Consolidate renderer duplicates only when semantics match |
| Extruded title mesh geometry | `title_mesh_extrusion.cpp`, `vulkan_text_renderer.cpp` | `title_mesh_extrusion.*` | Keep; renderer consumes generated vertices |
| FreeType lifetime, font lookup/loading, glyph metrics and rasterization | `vulkan_text_renderer.cpp` | `vulkan_text_font.cpp` | Create private renderer module |
| Text wrapping | `vulkan_text_renderer.cpp` | `vulkan_text_font.cpp` | Move with font metrics; do not create UI-specific wrappers |
| Vulkan text shaders, descriptors, pipelines, and primitive draw binding | `vulkan_text_renderer.cpp` | `vulkan_text_pipeline.cpp` | Create private renderer module |
| Atlas construction, glyph placement, cache storage, and GPU upload | `vulkan_text_renderer.cpp` | `vulkan_text_atlas.cpp` | Create private renderer module |
| Speaker-label layout and draw orchestration | `vulkan_text_renderer.cpp` | `vulkan_text_layout_speakers.cpp` | Create |
| Transcript layout and draw orchestration | `vulkan_text_renderer.cpp`, `direct_vulkan_preview_transcript.cpp` | Renderer layout in `vulkan_text_layout_transcript.cpp`; prepared-overlay data in existing direct-preview module | Create + Delegate |
| Flat and 3D title layout/draw orchestration | `vulkan_text_renderer.cpp` | `vulkan_text_layout_titles.cpp` | Create; delegate mesh geometry and shared styling |
| Renderer lifetime and top-level overlay dispatch | `vulkan_text_renderer.cpp` | `vulkan_text_renderer.cpp` | Keep |
| CPU overlay rendering | `cpu_overlay_render_backend.cpp` | Existing CPU backend | Keep; consume shared style, never Vulkan implementation details |
| Title/transcript/speaker style controls | `titles_tab.cpp`, `transcript_tab.cpp`, `speakers_tab.cpp`, `inspector_pane.cpp` | Respective tab/pane presentation modules | Keep UI orchestration; shared conversion remains in `overlay_text_style.*` |

The text split must preserve `tests/test_vulkan_text_generation.cpp`, direct
Vulkan parity coverage, font fallback, multiline layout, atlas/cache identity,
and flat/extruded title behavior. The uncommitted direct-render parity test is
user work and must not be overwritten.

## ImGui Application Shell

| Responsibility | Current location | Canonical owner | Decision |
|---|---|---|---|
| Startup, event loop, shutdown | `jcut_imgui_main.cpp` | `jcut_imgui_main.cpp` | Keep |
| X11/Vulkan/ImGui platform setup and input mapping | `jcut_imgui_main.cpp` | `jcut_imgui_platform.cpp` | Create |
| Font and UI preferences | `jcut_imgui_main.cpp` | `jcut_imgui_preferences.cpp` | Create |
| Runtime-control snapshots/screenshots/playhead | `jcut_imgui_main.cpp`, runtime-control server modules | `jcut_imgui_runtime_control.cpp` for adapter code; control-server modules for protocol | Create + Delegate |
| Preview/export worker orchestration | `jcut_imgui_main.cpp` | `jcut_imgui_workers.cpp` | Create |
| Media panel | `jcut_imgui_main.cpp` | `jcut_imgui_media_panel.cpp` | Create |
| Preview panel | `jcut_imgui_main.cpp` | `jcut_imgui_preview_panel.cpp` | Create |
| Timeline panel | `jcut_imgui_main.cpp` | `jcut_imgui_timeline_panel.cpp` | Create |
| Inspector/clips/tracks tables | `jcut_imgui_main.cpp` | `jcut_imgui_inspector_panel.cpp` | Create |
| Shared shell state/layout | `jcut_imgui_main.cpp` | `jcut_imgui_internal.h` | Create private boundary |
| Document serialization | `jcut_imgui_main.cpp`, editor document JSON modules | Existing editor document JSON/I/O modules | Move to existing | 

## Inspector and Grading UI

Recent history expanded `inspector_pane.cpp` with title/transcript styling and
grading controls. The pane owns widget composition; feature tabs own behavior.

| Responsibility | Current location(s) | Canonical owner | Decision |
|---|---|---|---|
| Top-level inspector pane and tab-container construction | `inspector_pane.cpp` | `inspector_pane.cpp` | Keep thin orchestration |
| Per-tab widget construction | `inspector_pane.cpp` | Existing feature-specific inspector files where present; otherwise `inspector_pane_<feature>_layout.cpp` | Move/Create after audit |
| Grading curve/histogram behavior and state | `grading_tab.cpp`, `grading_tab_curve.cpp`, `grading_histogram_widget.*` | Existing grading modules | Keep; inspector only constructs/injects widgets |
| Grading histogram painting and interaction | `grading_histogram_widget.*` | Existing widget module | Keep; preserve current uncommitted user work |
| Transcript overlay controls | `inspector_pane.cpp`, `transcript_tab.*`, `transcript_tab_overlay.cpp` | Transcript tab modules for behavior; inspector layout module for widget creation | Consolidate presentation wiring without duplicating style conversion |
| Title controls and behavior | `inspector_pane.cpp`, `titles_tab.*` | `titles_tab.*` for behavior; inspector title layout module for construction | Consolidate |
| Speaker controls and behavior | `inspector_pane.cpp`, speaker modules | Existing speaker modules for behavior; inspector speaker layout module for construction | Delegate |
| Shared overlay style conversion | inspector/tab files, `overlay_text_style.*` | `overlay_text_style.*` | Move to existing; UI files only gather/apply values |

## Face-Detection Offscreen Runner

| Responsibility | Current location(s) | Canonical owner | Decision |
|---|---|---|---|
| CLI option parsing and validation | runner, `vulkan_facedetections_offscreen_options.cpp` | `vulkan_facedetections_offscreen_options.*` | Move to existing |
| Top-level pipeline sequencing | runner | `vulkan_facedetections_offscreen_runner.cpp` | Keep, reduce to orchestration |
| Vulkan detector/harness execution | runner, `vulkan_facedetections_offscreen_vulkan_harness.cpp` | `vulkan_facedetections_offscreen_vulkan_harness.*` | Move to existing |
| Detection filtering | runner, `vulkan_facedetections_offscreen_detection_filters.cpp` | `vulkan_facedetections_offscreen_detection_filters.*` | Move to existing |
| Tuning | runner, `vulkan_facedetections_offscreen_tuning.cpp` | `vulkan_facedetections_offscreen_tuning.*` | Move to existing |
| Resume state | runner, `vulkan_facedetections_offscreen_resume_state.cpp` | `vulkan_facedetections_offscreen_resume_state.*` | Move to existing |
| Checkpoint writing | runner, `vulkan_facedetections_offscreen_checkpoint_writer.cpp` | Existing checkpoint writer | Move to existing |
| Artifact persistence | runner, `vulkan_facedetections_offscreen_artifact_io.cpp` | Existing artifact I/O | Move to existing |
| Preview/debug image I/O | runner, `vulkan_facedetections_offscreen_preview_io.cpp` | Existing preview I/O | Move to existing |
| Progress reporting | runner, `vulkan_facedetections_offscreen_progress.cpp` | Existing progress module | Move to existing |
| Decode/frame iteration not covered by existing modules | runner | `vulkan_facedetections_offscreen_decode.cpp` | Create only after confirming no decoder service fits |
| Detection/tracking stage adapters | runner and domain services | Runner-local pipeline stage functions | Delegate | Keep only data-flow adapters; detection and tracking algorithms stay in their existing services. |

The existing `vulkan_facedetections_offscreen_modularization.md` must be
reconciled with this table before runner changes begin. If it records a more
specific established boundary, update this matrix rather than creating a
competing module.

## Keyframes and Speaker Framing

| Responsibility | Current location(s) | Canonical owner | Decision |
|---|---|---|---|
| Editor commands that add/remove/mutate transform keyframes | `editor_transform_keyframe_ops.cpp` | Existing ops module | Keep |
| Editor commands for title/opacity keyframes | `editor_title_opacity_keyframe_ops.cpp` | Existing ops module | Keep |
| Generic transform normalization/interpolation/composition | `editor_shared_keyframes.cpp` | `editor_keyframes_transform.cpp` | Create |
| Grading normalization/evaluation | `editor_shared_keyframes.cpp` | `editor_keyframes_grading.cpp` | Create |
| Opacity normalization/evaluation | `editor_shared_keyframes.cpp` | `editor_keyframes_opacity.cpp` | Create |
| Title normalization/evaluation | `editor_shared_keyframes.cpp` | `editor_keyframes_titles.cpp` | Create |
| Shared evaluation caches | `editor_shared_keyframes_cache.cpp` | Existing cache module | Move to existing |
| Face-detection continuity artifacts | keyframes file, `facedetections_continuity_artifacts.cpp` | Existing artifact module | Move to existing |
| Speaker-framing runtime cache warm-up | `editor_shared_keyframes.cpp`, cache/runtime modules | `editor_speaker_framing_continuity.cpp` coordinating existing caches | Create + Delegate |
| Speaker target retargeting and transform evaluation | `editor_shared_keyframes.cpp` | `editor_speaker_framing_keyframes.cpp` | Create |
| Section-assignment JSON parsing | keyframes file and speaker files | `speaker_section_assignment_utils.*` | Consolidate |

## Pre-Move Audit Template

Complete one row for every extraction batch before editing source files:

| Source symbols / range | Responsibility | Existing candidates searched | Decision | Destination | Tests proving equivalence | Duplicates removed |
|---|---|---|---|---|---|---|
| _fill in_ | _fill in_ | _fill in_ | Keep / Move / Consolidate / Create / Delegate | _fill in_ | _fill in_ | _fill in_ |

## Completion Criteria

A responsibility is migrated only when:

1. its canonical owner is explicit in this document;
2. existing candidate modules were inspected;
3. duplicate behavior has characterization coverage;
4. all callers use the canonical implementation;
5. redundant implementations and declarations are removed;
6. no dependency cycle or broadened public header was introduced; and
7. focused tests, the full build, and `scripts/scan_redundant_code.sh` pass.
