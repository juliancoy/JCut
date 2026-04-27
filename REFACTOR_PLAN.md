# Refactor Plan

## Purpose

This document is the repo-specific execution plan for reducing large files under the 1500-line cap using the standards in `FILESIZE_REDUCTION_STRATEGY.md`.

## Baseline (2026-04-27)

Files currently over 1500 lines:

1. `editor.cpp` (~3150)
2. `speakers_tab_interactions.cpp` (~2961)
3. `control_server_worker_routes.cpp` (~2166)
4. `grading_tab.cpp` (~1795)
5. `preview_window_overlay.cpp` (~1620)
6. `inspector_pane.cpp` (~1611)

Files near threshold (proactive split target):

1. `timeline_widget_core.cpp` (~1461)
2. `editor_media_tools.cpp` (~1461)
3. `transcript_tab.cpp` (~1460)

## Step-by-Step Execution Plan

## Step 0: Preflight

Source files:

1. `CMakeLists.txt`

Destination files:

1. `CMakeLists.txt` (updated source lists only)

Actions:

1. Add placeholders for all destination files listed below.
2. Build after each step with `./build.sh`.

## Step 1: Split `editor.cpp`

Source files:

1. `editor.cpp`
2. `editor.h`

Destination files:

1. `editor_main.cpp`
2. `editor_cli.cpp`
3. `editor_ai_auth.cpp`
4. `editor_ai_actions.cpp`
5. `editor_state_load.cpp`
6. `editor_state_save.cpp`
7. `editor_shortcuts.cpp`
8. `editor_dialogs.cpp`

Actions:

1. Move `main()` and command-line parsing into `editor_main.cpp` and `editor_cli.cpp`.
2. Move AI login/token/entitlement state refresh into `editor_ai_auth.cpp`.
3. Move AI action submission wrappers into `editor_ai_actions.cpp`.
4. Move project load/migration into `editor_state_load.cpp`.
5. Move save/autosave/state snapshot code into `editor_state_save.cpp`.
6. Move shortcut registration and handlers into `editor_shortcuts.cpp`.
7. Move dialog construction/callbacks into `editor_dialogs.cpp`.
8. Keep only high-level orchestration glue in `editor.cpp`.

## Step 2: Split `speakers_tab_interactions.cpp`

Source files:

1. `speakers_tab_interactions.cpp`
2. `speakers_tab.h`
3. `speakers_tab_internal.h`

Destination files:

1. `speakers_tab_pipeline_transcript.cpp`
2. `speakers_tab_pipeline_candidates.cpp`
3. `speakers_tab_pipeline_matching.cpp`
4. `speakers_tab_pipeline_apply.cpp`
5. `speakers_tab_pipeline_debug.cpp`
6. `speakers_tab_interactions_ui.cpp`

Actions:

1. Move transcript normalization/read helpers into `speakers_tab_pipeline_transcript.cpp`.
2. Move candidate face detection and pre-crop logic into `speakers_tab_pipeline_candidates.cpp`.
3. Move speaker name to face matching logic into `speakers_tab_pipeline_matching.cpp`.
4. Move assignment writeback/mutation code into `speakers_tab_pipeline_apply.cpp`.
5. Move artifact generation and overwrite-prompt logic into `speakers_tab_pipeline_debug.cpp`.
6. Keep user-triggered handlers in `speakers_tab_interactions_ui.cpp`.
7. Keep shared constants/types in `speakers_tab_internal.h`.

## Step 3: Split `control_server_worker_routes.cpp`

Source files:

1. `control_server_worker_routes.cpp`
2. `control_server_worker.h`

Destination files:

1. `control_server_routes_health.cpp`
2. `control_server_routes_state.cpp`
3. `control_server_routes_ui.cpp`
4. `control_server_routes_media.cpp`
5. `control_server_routes_debug.cpp`

Actions:

1. Move `/health`, `/version` style routes into `control_server_routes_health.cpp`.
2. Move `/state`, `/project`, `/timeline`, `/tracks`, `/clips` routes into `control_server_routes_state.cpp`.
3. Move `/ui`, `/click`, `/click-item`, `/menu`, `/windows`, `/screenshot` routes into `control_server_routes_ui.cpp`.
4. Move decode/media diagnostics routes into `control_server_routes_media.cpp`.
5. Move `/debug`, profiling and perf routes into `control_server_routes_debug.cpp`.
6. Keep shared request parsing and route registration in `control_server_worker_routes.cpp`.

## Step 4: Split `grading_tab.cpp`

Source files:

1. `grading_tab.cpp`
2. `grading_tab.h`

Destination files:

1. `grading_tab_layout.cpp`
2. `grading_tab_bindings.cpp`
3. `grading_tab_state.cpp`
4. `grading_tab_actions.cpp`

Actions:

1. Move widget construction/layout into `grading_tab_layout.cpp`.
2. Move signal-slot wiring into `grading_tab_bindings.cpp`.
3. Move model-to-UI and UI-to-model synchronization into `grading_tab_state.cpp`.
4. Move command handlers and mutations into `grading_tab_actions.cpp`.

## Step 5: Split `preview_window_overlay.cpp`

Source files:

1. `preview_window_overlay.cpp`
2. `preview.h`

Destination files:

1. `preview_window_overlay_draw.cpp`
2. `preview_window_overlay_labels.cpp`
3. `preview_window_overlay_speakers.cpp`
4. `preview_window_overlay_debug.cpp`

Actions:

1. Move generic overlay paint orchestration into `preview_window_overlay_draw.cpp`.
2. Move text/label rendering into `preview_window_overlay_labels.cpp`.
3. Move speaker box/track visualization into `preview_window_overlay_speakers.cpp`.
4. Move debug overlays into `preview_window_overlay_debug.cpp`.

## Step 6: Split `inspector_pane.cpp`

Source files:

1. `inspector_pane.cpp`
2. `inspector_pane.h`

Destination files:

1. `inspector_pane_layout.cpp`
2. `inspector_pane_bindings.cpp`
3. `inspector_pane_state.cpp`
4. `inspector_pane_actions.cpp`

Actions:

1. Move tab and widget layout builders into `inspector_pane_layout.cpp`.
2. Move signal-slot and callback wiring into `inspector_pane_bindings.cpp`.
3. Move state refresh/apply logic into `inspector_pane_state.cpp`.
4. Move action handlers into `inspector_pane_actions.cpp`.

## Step 7: Proactive Near-Threshold Split (`timeline_widget_core.cpp`)

Source files:

1. `timeline_widget_core.cpp`
2. `timeline_widget.h`

Destination files:

1. `timeline_widget_layout.cpp`
2. `timeline_widget_state.cpp`
3. `timeline_widget_selection.cpp`

Actions:

1. Move geometry/layout calculations into `timeline_widget_layout.cpp`.
2. Move cached state/update logic into `timeline_widget_state.cpp`.
3. Move selection and selection-derived state changes into `timeline_widget_selection.cpp`.

## Step 8: Proactive Near-Threshold Split (`editor_media_tools.cpp`)

Source files:

1. `editor_media_tools.cpp`
2. `editor.h`

Destination files:

1. `editor_media_import.cpp`
2. `editor_media_proxy.cpp`
3. `editor_media_metadata.cpp`

Actions:

1. Move import/open media flows into `editor_media_import.cpp`.
2. Move proxy generation/toggle/use logic into `editor_media_proxy.cpp`.
3. Move metadata/probe/validation helpers into `editor_media_metadata.cpp`.

## Step 9: Proactive Near-Threshold Split (`transcript_tab.cpp`)

Source files:

1. `transcript_tab.cpp`
2. `transcript_tab.h`

Destination files:

1. `transcript_tab_layout.cpp`
2. `transcript_tab_bindings.cpp`
3. `transcript_tab_state.cpp`
4. `transcript_tab_actions.cpp`

Actions:

1. Move UI construction into `transcript_tab_layout.cpp`.
2. Move signal-slot bindings into `transcript_tab_bindings.cpp`.
3. Move state sync and table/model refresh into `transcript_tab_state.cpp`.
4. Move user action handlers into `transcript_tab_actions.cpp`.

## Validation Per Step

1. Build: `./build.sh`
2. Smoke run for startup: `./build/jcut` (or current binary path)
3. If route files changed: quick control-server endpoint smoke (`/health`, `/state`, `/ui`)
4. If speaker pipeline changed: run speaker debug artifact flow and verify outputs

## Completion Criteria

1. Every file above is below 1500 lines.
2. No destination file exceeds 1500 lines.
3. Behavior is unchanged except intended cleanup.
4. Build and smoke checks pass after each step.
