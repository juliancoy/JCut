# Refactor Plan (Line Cap: 1500)

## Scope and Rule
- Hard line cap per `.cpp` file: `1500`.
- Extraction-only refactors first: preserve behavior, APIs, and wiring.
- Build must pass after each extraction step.

## Current Status (2026-04-25)

### Completed
1. `control_server_worker.cpp` split
- Extracted HTTP routing + request handling into `control_server_worker_routes.cpp`.
- Result:
  - `control_server_worker.cpp`: `665`
  - `control_server_worker_routes.cpp`: `1425`
- Validation: `./build.sh --ninja` passed.

2. `editor.cpp` split
- Extracted playback/timing/runtime playback control methods into `editor_playback.cpp`.
- Result:
  - `editor.cpp`: `1392`
  - `editor_playback.cpp`: `621`
- Validation: `./build.sh --ninja` passed.

3. `inspector_pane.cpp` split
- Extracted secondary tab builders into `inspector_pane_secondary_tabs.cpp`:
  - `buildOutputTab`
  - `buildPreviewTab`
  - `buildClipTab`
  - `buildProfileTab`
  - `buildProjectsTab`
- Result:
  - `inspector_pane.cpp`: `1465`
  - `inspector_pane_secondary_tabs.cpp`: `459`
- Validation: `./build.sh --ninja` passed.

4. `timeline_widget_input.cpp` split
- Extracted `contextMenuEvent` into `timeline_widget_context_menu.cpp`.
- Result:
  - `timeline_widget_input.cpp`: `1041`
  - `timeline_widget_context_menu.cpp`: `658`
- Validation: `./build.sh --ninja` passed.

5. `timeline_cache.cpp` split
- Extracted storage-oriented implementations (`PlaybackBuffer` + `ClipCache`) into `timeline_cache_storage.cpp`.
- Result:
  - `timeline_cache.cpp`: `1334`
  - `timeline_cache_storage.cpp`: `354`
- Validation: `./build.sh --ninja` passed.

6. `transcript_tab.cpp` split
- Extracted transcript document/render-order/version management into `transcript_tab_document.cpp`.
- Result:
  - `transcript_tab.cpp`: `1386`
  - `transcript_tab_document.cpp`: `1102`
- Validation: `./build.sh --ninja` passed.

7. `speakers_tab.cpp` split
- Extracted into feature-focused files:
  - `speakers_tab_interactions.cpp` (UI interactions, reference editing, table/context actions)
  - `speakers_tab_boxstream_engines.cpp` (native/docker engine execution)
  - `speakers_tab_boxstream_actions.cpp` (boxstream orchestration + preview framing handlers)
  - `speakers_tab_internal.h` (shared internal helpers/constants)
- Result:
  - `speakers_tab.cpp`: `1139`
  - `speakers_tab_interactions.cpp`: `1254`
  - `speakers_tab_boxstream_engines.cpp`: `1111`
  - `speakers_tab_boxstream_actions.cpp`: `666`
- Validation: `./build.sh --ninja` passed.

## Naming Audit
- `inspector_pane_misc_tabs.cpp` -> `inspector_pane_secondary_tabs.cpp` (more precise).
- `transcript_tab_document_versions.cpp` -> `transcript_tab_document.cpp` (file contains broader document logic, not only versions).
- `speakers_tab_panel.cpp` -> `speakers_tab_interactions.cpp` (content is interaction-heavy, not only panel rendering).

## Final State
- No `.cpp` files exceed `1500` lines.
- Refactor objective complete.

## Change Log
- 2026-04-25: Added tracking document and recorded completed `control_server_worker` + `editor` splits.
- 2026-04-25: Completed `inspector_pane` extraction and validated build.
- 2026-04-25: Completed `timeline_widget_input` extraction and validated build.
- 2026-04-25: Completed `timeline_cache` extraction and validated build.
- 2026-04-25: Completed `transcript_tab` extraction and validated build.
- 2026-04-25: Completed `speakers_tab` extraction and validated build.
- 2026-04-25: Performed file naming audit and renamed extracted files for scope accuracy.
