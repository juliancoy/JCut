it # JCut Architecture

## Overview

JCut is a Qt6/C++20 non-linear editor with:
- Multi-track timeline editing
- Real-time OpenGL preview compositing
- FFmpeg-based async decode with timeline caching
- RtAudio playback and speech-filter support
- Offline render/export pipeline (GPU path + CPU fallback)
- Local control server for automation/diagnostics

Primary entry point: `editor.cpp` / `editor.h` (`EditorWindow`).

## Top-Level Runtime Layout

`EditorWindow` builds a 3-pane horizontal splitter (`editor_setup.cpp`):
- `ExplorerPane` (`explorer_pane.cpp`): media browser and ingest hooks
- `EditorPane` (`editor_pane.cpp` + `timeline_widget_*.cpp` + preview files): timeline + transport + preview
- `InspectorPane` (`inspector_pane.cpp`): tabbed tools and settings

State and project persistence are managed in `EditorWindow` via JSON snapshots/history, plus project switching helpers (`project_manager.*`, `project_state.*`, `projects.*`).

## Core Subsystems

### 1) UI + Interaction Layer
- Main window orchestration: `editor.cpp`, `editor_setup.cpp`, `editor_tabs.cpp`, `editor_input.cpp`
- Inspector construction and widget ownership: `inspector_pane.cpp`
- Specialized tab controllers:
  - `grading_tab.cpp`, `opacity_tab.cpp`, `effects_tab.cpp`, `corrections_tab.cpp`
  - `titles_tab.cpp`, `video_keyframe_tab.cpp`, `transcript_tab.cpp`
  - `output_tab.cpp`, `profile_tab.cpp`, `clips_tab.cpp`, `history_tab.cpp`
- Shared tab logic:
  - `keyframe_tab_base.*` for keyframe-style tables
  - `table_tab_base.*` for table refresh/sync behavior
  - `keyframe_table_shared.*` for row/frame selection helpers

### 2) Timeline + Data Model
- Core data structures: `editor_shared.h`
  - `TimelineClip`, `TimelineTrack`, render-sync markers, transcript settings
- Timeline widget implementation:
  - `timeline_widget_core.cpp`, `timeline_widget_input.cpp`, `timeline_widget_model.cpp`
  - `timeline_widget_layout.cpp`, `timeline_widget_paint.cpp`
- Timeline helpers:
  - `timeline_layout.*`, `timeline_renderer.*`, `track_sidebar.*`, `timeline_container.*`

### 3) Decode + Frame Supply
- Async decode scheduler: `async_decoder.*`
- FFmpeg decode contexts and codec helpers: `decoder_context.*`, `decoder_ffmpeg_utils.*`, `ffmpeg_compat.h`
- Image/still handling: `decoder_image_io.*`
- Frame wrapper and ownership: `frame_handle.*`

### 4) Caching + Playback Pipeline
- Timeline frame cache: `timeline_cache.*`, `timeline_cache_requests.cpp`, `timeline_cache_seek_resync.*`
- Playback frame pipeline: `playback_frame_pipeline.*`
- Budgeting/pressure: `memory_budget.*`
- Media pipeline shared utilities: `media_pipeline_shared.*`, `editor_shared_media.cpp`

### 5) Preview/Compositing
- Preview window and overlay interaction:
  - `preview_window_core.cpp`, `preview_window_gl.cpp`, `preview_window_overlay.cpp`
  - `preview_window_pipeline.cpp`, `preview_window_interaction.cpp`, `preview_window_transcript*.cpp`
- Rendering backend wrappers: `preview_renderer_backend.*`, `gl_frame_texture_shared.*`
- Optional/alternate compositor code exists in `gpu_compositor.*`.

### 6) Audio
- Audio playback/mixing engine: `audio_engine.h`
- Render-side audio export: `render_audio.cpp`
- Speech-filter parameters are driven from transcript settings (`transcript_tab.cpp` + engine usage in editor wiring).

### 7) Render/Export
- Render orchestration and request/result models: `render.h`, `render_internal.h`
- Decode/composite/export stages:
  - `render_decode.cpp`, `render_gpu.cpp`, `render_cpu_fallback.cpp`
  - `render_codecs.cpp`, `render_export.cpp`, `render_stats.cpp`

### 8) Control Server
- Front object/thread bootstrap: `control_server.*`
- Worker and HTTP routing: `control_server_worker.*`
- Utilities/diagnostics: `control_server_http_utils.*`, `control_server_media_diag.*`, `control_server_ui_utils.*`
- Provides local endpoints for health, state/profile snapshots, playhead control, diagnostics, and screenshots.

## Data Flow (Typical Playback)

1. Timeline frame changes (scrub/play).
2. Preview/pipeline requests frames from `TimelineCache`.
3. Cache miss triggers `AsyncDecoder` work.
4. Decoded frames are wrapped (`FrameHandle`) and cached.
5. Preview composites active layers and overlays.
6. Audio engine mixes timeline audio for the current playback sample position.

## Build + Dependencies

Defined in `CMakeLists.txt`:
- C++20, Qt6 (`Core`, `Gui`, `Widgets`, `Network`, `Concurrent`, `OpenGLWidgets`)
- FFmpeg (`libavcodec`, `libavformat`, `libavutil`, `libswscale`, `libswresample`)
- OpenGL + Threads
- RtAudio via bundled `rtaudio/`
- Optional AddressSanitizer (`EDITOR_ASAN`)
- Optional tests (`EDITOR_BUILD_TESTS`)

## Organizational Paradigms

- Vertical slice extraction:
  - Refactors split code by feature behavior (for example playback, routing, transcript document handling, speaker autotrack) rather than arbitrary line chunks.
- Separation of concerns:
  - UI interactions, orchestration, runtime/engine behavior, and storage/cache internals are isolated into different translation units.
- Facade + satellites:
  - Original files remain primary entry points while specialized implementations live in companion files.
- Single responsibility per translation unit:
  - Each extracted `.cpp` is scoped to one coherent responsibility.
- Shared internal helpers:
  - Reusable private helpers/constants can be centralized in internal headers to avoid duplication (for example `speakers_tab_internal.h`).
- Incremental strangler-style refactor:
  - Changes are applied in small, behavior-preserving extractions with build validation after each step.
- Constraint-driven modularization:
  - The line-cap constraint (`1500`) is used as a forcing function to improve modular boundaries.
- Behavior-preserving refactor discipline:
  - Extraction and reorganization first; no intentional feature changes during structural passes.

### File Positioning By Paradigm

This matrix assigns each refactored file to its position within the paradigms above.

Color key:
- `🟦` Facade/core owner
- `🟩` Satellite/extracted feature owner
- `🟪` Shared helper module/hub
- `🟧` Orchestration/lifecycle responsibility
- `🟥` Runtime engine responsibility
- `🟫` Storage/data-structure responsibility
- `🟨` UI/interaction responsibility

| File | Vertical Slice | Separation Of Concerns | Facade + Satellites | Single Responsibility | Shared Internal Helpers | Incremental Strangler | Constraint-Driven Modularization | Behavior-Preserving Refactor |
|---|---|---|---|---|---|---|---|---|
| `control_server_worker.cpp` | 🟦 Control-server core slice | 🟧 Worker lifecycle/cache/decode state | 🟦 Facade | 🟦 Worker state/lifecycle | 🟩 Uses shared worker types | 🟦 Retained while routes extracted | 🟩 Below cap | 🟩 Yes |
| `control_server_worker_routes.cpp` | 🟩 HTTP routing slice | 🟨 Route handlers + responses | 🟩 Satellite | 🟩 Endpoint routing | 🟩 Reuses worker internals | 🟩 Extracted from worker core | 🟩 Below cap | 🟩 Yes |
| `editor.cpp` | 🟦 Editor orchestration slice | 🟧 App startup/top-level wiring | 🟦 Facade | 🟦 App orchestration entrypoint | 🟩 Uses shared editor APIs | 🟦 Retained while playback extracted | 🟩 Below cap | 🟩 Yes |
| `editor_playback.cpp` | 🟩 Playback runtime slice | 🟥 Clocking/transport/runtime playback | 🟩 Satellite | 🟩 Playback behavior | 🟩 Reuses playback helpers | 🟩 Extracted from editor core | 🟩 Below cap | 🟩 Yes |
| `inspector_pane.cpp` | 🟦 Inspector composition slice | 🟧 Pane shell/tab assembly | 🟦 Facade | 🟦 Inspector container/wiring | 🟩 Uses tab contracts | 🟦 Retained while secondary tabs extracted | 🟩 Below cap | 🟩 Yes |
| `inspector_pane_secondary_tabs.cpp` | 🟩 Secondary inspector tab slice | 🟨 Output/Preview/Properties/System/Projects builders | 🟩 Satellite | 🟩 Secondary tab construction | 🟩 Reuses tab layout helpers | 🟩 Extracted from pane core | 🟩 Below cap | 🟩 Yes |
| `timeline_widget_input.cpp` | 🟦 Timeline input core slice | 🟨 Input/drag/mouse/wheel | 🟦 Facade | 🟦 Direct interaction path | 🟩 Uses timeline shared APIs | 🟦 Retained while menu extracted | 🟩 Below cap | 🟩 Yes |
| `timeline_widget_context_menu.cpp` | 🟩 Timeline context-menu slice | 🟨 Menu actions/commands | 🟩 Satellite | 🟩 Context-command surface | 🟩 Reuses widget state/mutators | 🟩 Extracted from input core | 🟩 Below cap | 🟩 Yes |
| `timeline_cache.cpp` | 🟦 Cache orchestration slice | 🟧 Prefetch/decode scheduling/playhead state | 🟦 Facade | 🟦 Cache control flow | 🟩 Uses storage classes via API | 🟦 Retained while storage extracted | 🟩 Below cap | 🟩 Yes |
| `timeline_cache_storage.cpp` | 🟩 Cache storage slice | 🟫 `PlaybackBuffer` + `ClipCache` storage ops | 🟩 Satellite | 🟫 Storage/eviction primitives | 🟩 Reused by orchestrator | 🟩 Extracted from cache monolith | 🟩 Below cap | 🟩 Yes |
| `transcript_tab.cpp` | 🟦 Transcript interaction slice | 🟨 Table/edit/context interactions | 🟦 Facade | 🟦 Interaction flow | 🟩 Uses document helpers via API | 🟦 Retained while document extracted | 🟩 Below cap | 🟩 Yes |
| `transcript_tab_document.cpp` | 🟩 Transcript document slice | 🟫 Load/parse/persist/version/render-order | 🟩 Satellite | 🟫 Document lifecycle + version mgmt | 🟩 Reuses transcript constants/helpers | 🟩 Extracted from transcript monolith | 🟩 Below cap | 🟩 Yes |
| `speakers_tab.cpp` | 🟦 Speakers core slice | 🟧 Wiring/refresh/summary/table model | 🟦 Facade | 🟦 Core orchestration | 🟪 Uses `speakers_tab_internal.h` | 🟦 Retained while feature files extracted | 🟩 Below cap | 🟩 Yes |
| `speakers_tab_interactions.cpp` | 🟩 Speakers interaction slice | 🟨 Selection/reference/context/panel actions | 🟩 Satellite | 🟨 Interaction-heavy behavior | 🟪 Uses internal helper hub | 🟩 Extracted from speakers monolith | 🟩 Below cap | 🟩 Yes |
| `speakers_tab_autotrack_engines.cpp` | 🟩 Speakers autotrack-engine slice | 🟥 Native/docker engine execution | 🟩 Satellite | 🟥 Engine execution layer | 🟪 Uses internal tracking utilities | 🟩 Extracted from speakers monolith | 🟩 Below cap | 🟩 Yes |
| `speakers_tab_autotrack_actions.cpp` | 🟩 Speakers autotrack-action slice | 🟧 Workflow orchestration + preview framing writes | 🟩 Satellite | 🟧 High-level action orchestration | 🟪 Uses internal helper hub | 🟩 Extracted from speakers monolith | 🟩 Below cap | 🟩 Yes |
| `speakers_tab_internal.h` | 🟪 Shared helper slice | 🟪 Cross-file constants/private helper algorithms | 🟪 Shared helper module | 🟪 Internal helper utilities only | 🟪 Primary helper hub | 🟩 Introduced during split | 🟩 Prevents duplication/regrowth | 🟩 Yes |

## Notes

- `ARCHITECTURE.md` is intended as an implementation map, not a roadmap.
- If tabs, endpoints, or pipeline ownership move, update this document alongside code changes.
