# Dear ImGui Parallel Implementation Plan

## Goal

Add a non-Qt Dear ImGui implementation of JCut while keeping the existing Qt editor working.

Shared functionality should move behind Qt-free subsystem APIs, with Qt and ImGui acting as separate shells over the same editor/runtime core.

The ImGui build must use the same project root, active-project marker, `state.json`, and `history.json` files as the Qt build, and the target end state is full UI coverage rather than a reduced shell.

## Progress Update

Completed in the current implementation:

- `jcut_imgui` is a separate Qt-free executable.
- ImGui project/session loading uses the same active-project and state/history files as the Qt build.
- ImGui preview rendering no longer links the Qt render path.
- ImGui export now has a Qt-free standalone FFmpeg backend and no longer depends on the Qt exporter.
- Standalone export now supports encoded video plus image-sequence output in the ImGui build.
- `ImGuiPreviewWindow` no longer exposes Qt string/size state internally; remaining Qt coupling there is limited to upstream geometry/handoff boundaries.
- `VulkanDetectorFrameHandoff` now exposes neutral `std::string` error plumbing to its callers, with Qt conversions kept inside detector-facing implementations and Qt shells.

Still remaining for full parity:

- move richer composition, overlays, transcript rendering, and effect evaluation behind the same neutral renderer contract
- carry the remaining Qt-bound subsystem surfaces out of shared headers
- close behavioral gaps between the current standalone renderer and the full Qt/Vulkan renderer

## Key Constraint

Do not start by replacing the Qt UI.

First create neutral boundaries. The current code still exposes Qt types in important non-UI places, including render/export models, preview surfaces, transcript helpers, JSON models, and render/export paths.

## Target Architecture

Create three layers:

1. Core/runtime layer
   - No Qt dependency.
   - Owns timeline state, project state, decode, playback, rendering, export, transcript logic, face detection, cache, audio, and command execution.
   - Uses standard C++ types, neutral image/frame structs, and `nlohmann::json` or another non-Qt JSON type.

2. UI adapter layer
   - Thin bridge between a UI framework and the runtime.
   - Converts UI input into editor commands.
   - Converts runtime snapshots into view models.
   - Has two implementations:
     - `jcut_qt_adapter`
     - `jcut_imgui_adapter`

3. Shell layer
   - Existing Qt application remains as `jcut_qt`.
   - New Dear ImGui/GLFW/Vulkan application becomes `jcut_imgui`.
   - Both link the same runtime libraries.

## Phase 0: Inventory And Boundaries

Create a Qt-coupling inventory by category:

- UI-only Qt: `QWidget`, `QMainWindow`, `QAction`, `QPainter`, `QShortcut`
- eventing/threading: `QObject`, signals/slots, `QTimer`, `QThread`, `QMutex`
- data types: `QString`, `QVector`, `QHash`, `QSet`, `QSize`, `QRectF`
- JSON/filesystem: `QJsonObject`, `QJsonArray`, `QFile`, `QDir`, `QFileInfo`
- image/render interchange: `QImage`, `QPainter`
- IPC/network: Qt socket/server types

Output of this phase should be a short migration matrix listing each module as:

- keep Qt UI-only
- adapt behind interface
- migrate to neutral core
- defer

Start with these high-value areas:

- `render.h`
- `render_internal.h`
- `preview_surface.h`
- `editor_shared_*`
- `transcript_document_*`
- `timeline_cache.*`
- `playback_frame_pipeline.*`
- `audio_engine.*`
- `project_state.*`
- `projects.*`
- `control_server_*`

## Phase 1: Create Qt-Free Shared Types

Introduce a neutral shared type layer, for example:

- `core/jcut_string.h` only if needed, otherwise prefer `std::string`
- `core/geometry.h`
  - `Size`
  - `Point`
  - `Rect`
  - `Transform`
- `core/image_buffer.h`
  - pixel format
  - width/height/stride
  - byte storage/view
- `core/frame_buffer.h`
  - CPU frame buffer
  - GPU/Vulkan frame handle metadata
- `core/json.h`
  - likely `nlohmann::json`
- `core/result.h`
  - status/error/result helpers
- `core/time.h`
  - frame/sample/time conversions

Then replace boundary-facing Qt types gradually:

- `QString` -> `std::string`
- `QVector<T>` -> `std::vector<T>`
- `QHash/QMap` -> `std::unordered_map` / `std::map`
- `QSet` -> `std::unordered_set`
- `QSize/QRectF` -> neutral geometry structs
- `QJsonObject/QJsonArray` -> neutral JSON
- `QImage` -> `ImageBuffer` or explicit compatibility wrapper

Qt types can remain inside Qt UI files, but not in shared subsystem headers.

## Phase 2: Split Build Targets

Refactor CMake into clear targets:

- `jcut_core`
  - timeline model
  - project/session state
  - transcript services
  - render request/result models
  - shared command system
  - no Qt linkage

- `jcut_media`
  - FFmpeg decode
  - timeline cache
  - playback frame pipeline
  - audio engine
  - no Qt linkage unless temporarily isolated behind adapter files

- `jcut_render`
  - Vulkan/offscreen renderer
  - CPU fallback
  - overlay backend
  - no Qt as a public dependency

- `jcut_qt_ui`
  - current Qt widgets and adapters

- `jcut_imgui_ui`
  - Dear ImGui UI implementation

- executables:
  - `jcut_qt`
  - `jcut_imgui`
  - existing test/offscreen tools

The existing `imgui_preview_window.cpp` can be reused, but its header should be de-Qt'd because it currently exposes `QString`, `QSize`, `QRectF`, and `QVector`.

## Phase 3: Introduce A Command-Based Editor Core

Create a runtime facade, for example:

```cpp
class EditorRuntime {
public:
    EditorSnapshot snapshot() const;
    CommandResult execute(const EditorCommand& command);
    void tick(const TickParams& params);
    void requestPreviewFrame(int64_t timelineFrame);
    RenderResult render(const RenderRequest& request, ProgressCallback cb);
};
```

Commands should cover user intent, not UI events:

- open project
- save project
- import media
- select clip
- move/trim clip
- split clip
- delete clip
- update clip property
- seek timeline
- play/pause
- set preview zoom/pan
- update transcript
- update speaker assignment
- start export
- cancel export

Both Qt and ImGui send the same commands.

This prevents duplicating business logic in the new UI.

## Phase 4: Decouple Rendering And Preview

Make GPU/native frame output the primary render contract.

Current risk areas:

- `RenderProgress::previewFrame` uses `QImage`
- `RenderRequest` and `RenderResult` use Qt types
- `render_export.cpp` still has `QPainter`, `QImage`, `QFile`, `QDir`
- `OffscreenRenderFrame` still carries compatibility CPU image paths

Plan:

1. Create Qt-free `RenderRequestCore`, `RenderResultCore`, and `RenderProgressCore`.
2. Keep Qt wrappers temporarily:
   - `RenderRequest fromQtRenderRequest(...)`
   - `QtRenderResult toQtRenderResult(...)`
3. Make readback explicit:
   - `RenderOutputMode::GpuFrame`
   - `RenderOutputMode::CpuImage`
   - `RenderOutputMode::EncodedFile`
4. Keep `QImage` only in Qt compatibility files.
5. Push `OverlayImage`/raw RGBA buffers into Vulkan composition instead of `QPainter`.

Exit criteria:

- ImGui preview/export does not require `QImage`.
- Qt export still works through adapter shims.
- strict Vulkan paths fail loudly on unexpected CPU materialization.

Status:

- standalone ImGui preview is now Qt-free
- standalone ImGui export is now Qt-free
- shared render contracts still need deeper parity work before this phase is fully complete across all renderer features

## Phase 5: Decouple Project, Transcript, And JSON Services

Move project and transcript operations out of Qt JSON APIs.

Suggested neutral services:

- `ProjectStore`
- `ProjectSerializer`
- `TranscriptDocumentService`
- `TranscriptEditService`
- `SpeakerAssignmentService`
- `TimelineSerializer`

Use filesystem APIs from C++17/20:

- `std::filesystem::path`
- `std::ifstream`
- `std::ofstream`

Use neutral JSON at subsystem boundaries.

Qt UI can still convert to/from `QJsonObject` only at the adapter edge if needed.

## Phase 6: Decouple Eventing, Timers, And Threading

Do not expose `QObject`, signals, slots, or `QTimer` from runtime services.

Replace public-facing eventing with:

- callbacks
- observer interfaces
- lock-free or mutex-protected queues
- `std::thread`
- `std::future`
- `std::condition_variable`
- runtime `tick()` calls driven by the shell

Qt shell can map Qt signals to commands.

ImGui shell can poll runtime state every frame and submit commands directly.

## Phase 7: Build The Dear ImGui Shell

Create a new executable, likely `jcut_imgui`.

Recommended stack:

- GLFW for window/input
- Vulkan backend for rendering
- Dear ImGui docking branch/features if available
- existing Vulkan preview handoff where possible

Initial ImGui layout:

- top menu/toolbar
- left media/project browser
- center preview
- bottom timeline
- right inspector
- transcript/speakers/effects tabs as dockable panels
- export/progress modal or dock panel

The final ImGui build must include the entire Qt UI surface. Partial shells are acceptable only as migration checkpoints while shared subsystems are being extracted.

First usable milestone:

1. launch without Qt
2. open project/media
3. show timeline snapshot
4. seek/play/pause
5. show Vulkan preview
6. select clip
7. edit basic clip properties
8. export using shared render subsystem

## Phase 8: Keep Qt Working In Parallel

The existing Qt editor should continue to build throughout the migration.

For every decoupled subsystem:

- migrate core logic to Qt-free implementation
- keep Qt adapter with old public behavior
- add tests against the Qt-free core
- only then update Qt UI call sites

Avoid a long-lived fork where Qt and ImGui each own different business logic.

## Phase 9: Testing And Parity

Add parity tests around shared subsystems:

- project load/save roundtrip
- timeline edit commands
- transcript edit commands
- render request serialization
- preview frame selection
- export result/progress reporting
- overlay layout/raster output
- Vulkan no-readback strict path

Add shell smoke tests:

- Qt launches
- ImGui launches
- both can open the same project
- both produce equivalent runtime snapshots after the same command script

## Recommended Implementation Order

1. Add neutral core types.
2. Split CMake targets into Qt-free core/render/media libraries and UI-specific shells.
3. Convert `render.h` request/result/progress models away from Qt.
4. Convert project/transcript JSON services away from Qt.
5. Extract `EditorRuntime` and command/snapshot APIs.
6. Port Qt UI to use the runtime facade.
7. De-Qt the existing `ImGuiPreviewWindow`.
8. Create `jcut_imgui` executable.
9. Implement ImGui timeline, preview, inspector, and export panels.
10. Add parity tests and strict fallback checks.

## Main Rule

Shared functionality must move downward into Qt-free subsystems. Qt and Dear ImGui should only contain presentation, input handling, and adapter code.

Any logic duplicated between the two shells should be treated as a migration bug.

## Progress Update

- Completed another backend-decoupling phase focused on Vulkan frame/image contracts.
- `render_detail::OffscreenVulkanFrame`, `jcut::vulkan_detector::VulkanExternalImage`,
  `DirectVulkanFrameHandoffPipeline::Result`, and decoder-preparation size plumbing now use
  `jcut::core::SizeI` instead of `QSize`.
- Added neutral equality/operators on `jcut::core::SizeI` and kept Qt conversions at shell/runtime
  edges only.
- Updated Qt-side consumers to convert explicitly at the boundary:
  `direct_vulkan_preview_window.cpp`, `direct_vulkan_frame_handoff_pipeline.cpp`,
  `facedetections_runtime.cpp`, and `vulkan_facedetections_offscreen_runner.cpp`.
- Verified after the change:
  - `cmake --build build -j2 --target jcut_vulkan_facedetections_offscreen`
  - `cmake --build build -j2 --target editor`
  - `cmake --build build -j2 --target jcut_imgui`
  - `ctest --test-dir build -R 'test_imgui_binary_no_qt|test_imgui_standalone_render|test_imgui_standalone_export' --output-on-failure`
- Remaining work on this track is to remove the larger Qt request/result/JSON and renderer-surface
  types that still sit above these now-neutral Vulkan/backend contracts.

## UI Parity Audit

### Conclusion

The Dear ImGui editor is not yet fully on par with the Qt editor. It is a usable migration
checkpoint with the main editor layout and a functional basic editing loop, but a substantial
part of the visible inspector surface is currently scaffolding rather than connected editor
functionality. It should not yet be treated as a drop-in replacement for the Qt UI.

### Current Functional Coverage

The following areas have meaningful working implementations in the ImGui shell:

- loading and saving the active project session
- browsing and importing media
- selecting tracks and clips
- basic clip insertion and deletion
- timeline seeking
- clip move, start trim, and end trim interactions
- splitting a selected clip at the playhead
- playback, pause, frame stepping, and playback-speed control
- basic audio-preview integration
- Vulkan preview rendering
- basic preview zoom and pan
- export size, frame rate, path, container, proxy, and image-sequence settings
- starting, monitoring, cancelling, and reporting standalone exports
- basic project, clip, track, and panel-visibility commands

### Feature Comparison

| Area | ImGui status | Remaining parity work |
| --- | --- | --- |
| Project loading and saving | Partially functional | Add full project selection, creation, rename, history, and safe roundtrip coverage. |
| Media browser and import | Mostly functional | Match Qt metadata, drag/drop, proxy, deletion, and media-management actions. |
| Playback and seek | Functional basic implementation | Match Qt shortcuts, playback policies, preview controls, and advanced transport behavior. |
| Timeline | Basic select, move, trim, split, and delete work | Add multi-selection, undo/redo, razor mode, nudge, copy/paste, snapping behavior, track reorder, context actions, export ranges, proxies, transcripts, and facedetection actions. |
| Preview | Functional basic Vulkan preview | Close renderer gaps for richer composition, overlays, transcripts, grading, masks, effects, and exact Qt/Vulkan behavior. |
| Export | Basic video and image-sequence export works | Match the complete Qt output configuration and renderer behavior. |
| Grade | Mostly presentation-only | Bind grading values, channel curves, histograms, keyframes, interpolation, preview, and auto-oppose behavior. |
| Opacity | Presentation-only | Bind opacity values, keyframes, interpolation, and fade operations. |
| Effects | Presentation-only | Bind effect state, mask feathering, effect parameters, presets, and preview evaluation. |
| Masks | Missing as an equivalent inspector tab | Port the full masks UI and editing workflow. |
| Corrections | Presentation-only | Bind enablement, polygon drawing, ranges, point editing, clearing, and preview overlays. |
| Titles | Presentation-only | Bind title creation, removal, text/style controls, placement, keyframes, and preview rendering. |
| Sync | Presentation-only | Load, edit, clear, and display actual sync points and associated actions. |
| Transform | Presentation-only | Bind transform values, keyframes, interpolation, flips, and preview manipulation. |
| Transcript | Visibility toggle only; editor table is empty | Port transcript loading, editing, timing, skipping, restoration, overlay styling, and context actions. |
| Speakers | Presentation-only | Port roster, sections, assignments, AI cleanup, continuity tracks, face detections, titles, and debugging tools. |
| History | Empty table | Add actual undo/redo history and navigation. |
| Audio | Basic status and waveform toggle | Port the full audio inspector, routing, dynamics, mixing, isolation, and preview controls. |
| Processing jobs | Missing as an equivalent inspector tab | Port job creation, progress, cancellation, diagnostics, and result handling. |
| AI Assist | Presentation-only | Connect transcription, transcript mining, model selection, chat, and cleanup actions. |
| Access | Presentation-only | Connect entitlement/subscription data and refresh behavior. |
| Scopes | Placeholder drawing | Feed real grading scope and histogram data. |
| Pipeline | Placeholder drawing and static labels | Connect the actual pipeline graph, state, diagnostics, and preview. |
| System | Mostly static status | Connect decoder threading, benchmarks, decoder restart, hardware status, and diagnostics. |
| Projects | Current path display and Save As only | Implement new, rename, switch, and project-management behavior. |
| Preferences | Small functional subset | Port the complete Qt preferences surface and persistence behavior. |

### Visible But Unwired Controls

Several tabs currently resemble their Qt counterparts but do not apply changes to the runtime.
Examples in `jcut_imgui_main.cpp` include:

- saturation, brightness, and contrast sliders
- grade keyframe and auto-oppose buttons
- opacity slider, keyframe, and fade buttons
- mask-feathering controls
- correction toggles and polygon buttons
- title text, placement, size, add, and remove controls
- sync-point clearing
- transform sliders, keyframe creation, and horizontal flip
- transcript layout controls and transcript rows
- speaker mining, organization, cleanup, continuity, and detection controls
- AI transcription, mining, chat send, and clear actions
- access refresh
- decoder threading, benchmark, and restart controls
- project New and Rename actions

Many of these controls use temporary local values that are recreated every frame, or their return
values are ignored. They therefore provide visual coverage without behavioral parity.

### Menu And Shortcut Gaps

The File menu currently displays disabled Open Project, Import Media, and Export entries even
though some related operations are available elsewhere in the shell. Save and Reload are
available through the menu, but their displayed `Ctrl+S` and `Ctrl+R` shortcuts are not handled as
editor actions.

The ImGui shell also lacks the Qt editor's major editing shortcuts and commands, including:

- undo and redo
- split at playhead shortcut
- razor/select tool switching
- delete-selected shortcut
- left/right clip nudging
- spacebar playback toggle
- timeline copy, cut, and paste
- context-sensitive timeline actions

### Project And History Risk

The ImGui project adapter preserves a legacy state root while translating the reduced
`EditorDocumentCore` model back to the Qt project format. Existing legacy clip fields can be
preserved when clips are matched, but the neutral ImGui document model does not represent the
full Qt project model.

More importantly, `saveImGuiProjectSession(...)` currently rewrites `history.json` with one entry
at index zero. Saving through ImGui therefore does not preserve the Qt editor's full history stack.
This must be resolved and covered by roundtrip tests before the ImGui shell is considered safe for
full interchange with complex Qt projects.

### Data Model And Runtime Gaps

`EditorDocumentCore` currently contains only basic media, track, clip, transport, panel, and export
state. `EditorRuntime` likewise exposes a deliberately small command set centered on basic editing
and export configuration. Full parity requires neutral models and commands for at least:

- undo/redo and history transactions
- complete clip timing and source-time state
- multi-clip selection and editing
- track flags, ordering, audio, and compositing state
- transform, opacity, grading, and title keyframes
- effects, masks, and corrections
- sync markers and export ranges
- transcripts and transcript overlay style
- speakers, assignments, continuity tracks, and face detections
- audio mixing and processing
- project management and preferences
- processing jobs, AI actions, access state, and diagnostics

These capabilities should be added to the shared runtime rather than implemented as ImGui-only
business logic.

### Layout And Interaction Gaps

The current ImGui shell reproduces the broad left-media, center-preview, bottom-timeline, and
right-inspector arrangement. Its panels are positioned and sized by the shell each frame and are
marked non-movable and non-resizable. This does not yet match the Qt editor's adjustable splitters,
richer focus behavior, drag/drop interactions, context menus, and keyboard workflow. Docking or an
equivalent persisted adjustable layout remains part of full UI parity.

### Test Coverage Assessment

Current ImGui-specific tests primarily cover binary linkage and standalone preview/export paths.
They do not establish full UI parity. The following automated coverage is still required:

- command-level Qt/ImGui state equivalence for every editor operation
- complex Qt project load/save roundtrips without state or history loss
- shell interaction tests for every functional ImGui control
- detection of visible but unwired controls
- undo/redo and history parity
- advanced timeline interaction parity
- transcript, speaker, effect, mask, correction, title, grading, and audio parity
- equivalent preview output after identical command sequences
- equivalent export output and configuration behavior
- persisted layout, preferences, and keyboard-shortcut behavior

### Priority Order For Closing UI Parity

1. Protect project data and preserve the complete history stack during ImGui saves.
2. Expand the neutral document model and runtime command API to represent the full Qt editor state.
3. Add undo/redo, shortcuts, multi-selection, copy/paste, nudge, razor, snapping, and context actions.
4. Replace presentation-only Grade, Opacity, Effects, Corrections, Titles, Sync, and Transform tabs with bound controls.
5. Port Masks and Processing Jobs, which currently lack equivalent ImGui inspector tabs.
6. Port complete Transcript and Speaker workflows, including AI and facedetection integrations.
7. Port complete Audio, Output, Projects, Preferences, Access, Pipeline, and System behavior.
8. Feed real data to scopes, histograms, pipeline views, history tables, and other placeholders.
9. Match Qt layout flexibility, focus, drag/drop, context menus, and keyboard behavior.
10. Add shell-level and cross-shell parity tests, then remove the migration-checkpoint designation only when they pass.

### Parity Exit Criteria

The ImGui UI can be considered on par with the Qt UI only when:

- every Qt inspector tab has an ImGui equivalent with connected behavior
- every visible ImGui editor control reads and writes persistent runtime state
- the same editing operations produce equivalent project snapshots in both shells
- complex projects roundtrip between shells without losing state or history
- preview and export results are equivalent for the same project and settings
- shortcuts, timeline editing, drag/drop, context actions, and undo/redo are functionally equivalent
- the ImGui shell has no placeholder tables, graphs, or buttons presented as working editor features
- shell smoke tests and shared parity tests cover the complete supported UI surface
