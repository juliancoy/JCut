# JCut Dataflow

## Purpose
This document describes the current dataflow in JCut. It is a working
architecture note, not a target-state plan.

JCut currently uses a pragmatic Qt-centered dataflow:

`UI event -> domain mutation -> cache/runtime sync -> persistence schedule -> inspector refresh`

Some subsystems are already guarded and deferred, but the codebase is not yet a
pure reducer/store architecture. Treat `EditorWindow`, `TimelineWidget`,
`InspectorPane`, and tab controllers as the active integration layer.

## Canonical State

### Project State
Project folders live under:

`rootDirPath()/projects/<project-id>/`

The active project id is stored in:

`rootDirPath()/projects/.current_project`

Project state is persisted to:

`projects/<project-id>/state.json`

`EditorWindow::buildStateJson()` is the canonical serializer. It captures:

- media root/gallery and explorer expansion state
- timeline clips, tracks, export ranges, render sync markers, selection, zoom,
  scroll, and current frame
- output/render/preview settings
- transcript, speakers, grading, keyframe, AI, audio, and autosave UI settings
- selected inspector tab

Dense generated tracking data is intentionally excluded from project state.
`speakerFramingKeyframes` is stripped from timeline clips before state/history
serialization.

### History State
Undo/redo history is persisted to:

`projects/<project-id>/history.json`

The structure is:

```json
{ "entries": [ ...state snapshots... ], "index": 0 }
```

`pushHistorySnapshot()` builds a state snapshot, strips heavy fields, dedupes
against the current history entry, truncates redo entries, appends the new
snapshot, and caps the list at 200 entries.

History writes are delayed through `m_historySaveTimer`. `undoHistory()`,
`redoHistory()`, and `restoreToHistoryIndex()` apply snapshots with
`m_restoringHistory` set so the restore itself does not create a new snapshot.

### Transcript State
Transcript data is stored outside `state.json` in the active transcript JSON for
the selected clip. `TranscriptEngine` is the read/write boundary for transcript
documents and companion files.

Tabs keep loaded transcript state in memory while selected:

- `TranscriptTab::m_loadedTranscriptDoc`
- `SpeakersTab::m_loadedTranscriptDoc`
- `m_loadedTranscriptPath`
- `m_loadedClipFilePath`

Edits to transcript or speaker metadata commonly write the transcript JSON
directly, then emit `transcriptDocumentChanged()`, schedule project state save,
push history if appropriate, and refresh affected views.

### FaceDetections Artifacts
FaceDetections continuity metadata is stored as transcript sidecars, not inside
`state.json` or history snapshots. Dense generated artifacts are stored in a
media-adjacent clip sidecar directory.

Primary helpers are in `facedetections_runtime.cpp` and `TranscriptEngine`:

- raw artifact schema: `jcut_facedetections_v1`
- processed artifact schema: `jcut_facedetections_processed_v1`
- top-level map: `continuity_facedetections_by_clip`

The raw artifact stores generated continuity roots by clip id. The processed
artifact stores derived `streams` by clip id. Readers use the canonical
FaceDetections sidecars; transcript-side continuity payloads are not supported.

Large generated files such as `facedetections.part`, `tracks.idx/tracks.dat`,
`detections.idx/detections.dat`, `continuity_facedetections.bin`, and
`summary.json` live under:

`<media_dir>/<media_stem>.jcut/facedetections/<clip_id>/`

Persistent continuity-track avatar crops live under the same media sidecar root:

`<media_dir>/<media_stem>.jcut/track_memory/<clip_id>/`

`debug/speaker_flow` is only for request/index diagnostics and must not be the
runtime storage contract for clip FaceDetections artifacts.

## Runtime State

These objects are operational state, not canonical persisted state:

- `TimelineWidget` live clip/track/selection bindings
- preview surface state and preview-only toggles
- `TimelineCache` visible/prefetch caches
- `PlaybackFramePipeline` playback buffers
- `AsyncDecoder` in-flight decode requests and ready frames
- tab-local loaded documents such as `m_loadedTranscriptDoc`
- audio engine playback position/runtime configuration

The rule is simple: runtime state may be rebuilt from canonical persisted state
plus current media files. It should not become a second source of truth.

## Startup Flow

1. `EditorWindow::loadState()` loads projects from folders and selects the
   active project id.
2. It reads `history.json`. If valid entries exist, it uses the selected history
   entry as the startup root. Heavy fields are sanitized in memory and the
   history file is rewritten if needed.
3. If no history snapshot is available, it reads `state.json`.
4. `applyStateJson()` parses scalar settings, timeline clips/tracks/export
   ranges/render sync markers, selected clip, output settings, debug settings,
   feature flags, and tab preferences.
5. During apply:
   - `m_loadingState` suppresses saves and history snapshots.
   - widget value restores use `QSignalBlocker` where signals would otherwise
     cause feedback.
   - timeline callbacks are temporarily detached while clips/tracks are rebound.
   - preview, audio engine, timeline cache inputs, and playback ranges are
     synchronized from the loaded timeline.
6. On initial startup, seek/audio positioning is deferred through
   `QTimer::singleShot(0, ...)` so first render setup can complete.
7. The deferred startup callback sets the media root, reloads projects, refreshes
   the project list, and refreshes the inspector.
8. `loadState()` ensures transcript `.txt` companions exist for known transcript
   paths, creates an initial history snapshot if none exists, then schedules a
   state save.

Startup profiling checkpoints are emitted with `startupProfileMark()` around
project load, history read, state apply, timeline binding, preview binding, audio
binding, and seek.

## Save Flow

State saves are timer/debounce based:

- `scheduleSaveState()` exits during load or if the timeline is missing.
- While playback is active it sets `m_pendingSaveAfterPlayback` and avoids JSON
  serialization on the playback path.
- Otherwise it starts `m_stateSaveTimer`.
- `saveStateNow()` serializes `buildStateJson()`, skips writing if the payload is
  unchanged from `m_lastSavedState`, and writes with `QSaveFile`.

Project switching and "Save Project As" force `saveStateNow()` and
`saveHistoryNow()` before changing project folders.

Autosave writes timestamped `state_backup_*.json` files and keeps only the most
recent `m_autosaveMaxBackups` files.

## Inspector Refresh Flow

`InspectorPane::refreshRequested` is routed by
`EditorWindow::setupInspectorRefreshRouting()` to each tab refresh method:

- grading, opacity, effects, corrections, titles, sync
- transcript and speakers
- properties, video keyframes, output, pipeline, profile, projects
- clips, history, tracks

The refresh router records the last and max refresh durations and increments a
slow-refresh counter for refreshes over 30 ms.

Refresh methods are still responsible for guarding themselves. Current patterns:

- `TableTabBase::m_updating` suppresses handlers while a table is being rebuilt.
- Tabs use `QSignalBlocker` for control restores and selected bulk updates.
- `SpeakersTab` preserves the selected speaker id where possible.
- `SpeakersTab` queues the FaceDetections paths panel refresh through a single-shot
  40 ms timer to collapse bursts.
- `TranscriptTab` tracks manual-selection state so playhead follow does not fight
  user table selection.

Do not assume refresh is pure. Some current tab refresh paths load transcript
JSON, clear/repopulate tables, refresh preview-related controls, and update local
document caches.

### Refresh Invariants

Use these as code review checks for any new or modified `refresh()` path:

1. `refresh()` may rebuild widget state and reload local documents needed for
   display.
2. `refresh()` must not write `state.json`, `history.json`, transcript JSON, or
   FaceDetections artifacts.
3. `refresh()` should preserve the current logical selection when the selected
   object still exists.
4. Bulk widget rebuilds must run under `m_updating`, `QSignalBlocker`, or an
   equivalent guard when connected handlers can mutate state.
5. Expensive repeated refresh triggers should be collapsed with a timer or other
   debounce when they originate from playhead movement, worker bursts, or
   selection churn.

## Edit Flow

Timeline mutations generally follow this pattern:

1. User action mutates `TimelineWidget` clips/tracks/selection.
2. Editor callbacks synchronize preview/audio/cache state.
3. Inspector tabs refresh if selection or clip properties changed.
4. `scheduleSaveState()` persists the new project state.
5. `pushHistorySnapshot()` records an undo snapshot for finalized edits.

When adding new edit actions, be explicit about the persistence boundary:

- if the action changes project-visible timeline/editor state, it belongs in
  `state.json`
- if the action must participate in undo/redo, it must define when
  `pushHistorySnapshot()` happens
- if the action changes transcript semantics, it must go through
  `TranscriptEngine`
- if the action produces dense/generated tracking output, it belongs in a
  sidecar, not project state/history

## Playback Flow

Playback-sensitive paths avoid heavy synchronous work:

- `scheduleSaveState()` defers saves while playback is active.
- `TimelineCache` and `PlaybackFramePipeline` receive clip lists, export ranges,
  render sync markers, playback state, and playhead frame updates.
- Both cache layers drop stale visible requests as the playhead advances.
- Decode and prefetch work is async through `AsyncDecoder`; UI code consumes
  ready `FrameHandle` objects and cached presentation frames.
- Inspector refresh duration is monitored so playback warnings can identify UI
  churn.

Playback hot-path rule: do not add JSON serialization, transcript writes,
history writes, broad inspector refreshes, or blocking file I/O to code that
runs because the playhead advanced.

## Transcript and Speakers Flow

`TranscriptTab::refresh()`:

1. Clears the table and local transcript state under `m_updating`.
2. Resolves the selected clip.
3. Loads the active transcript JSON for audio-capable clips.
4. Updates overlay controls from the selected clip.
5. Rebuilds table rows and follow ranges from the transcript document.

Transcript edits write the transcript JSON through `TranscriptEngine`, refresh
the table, schedule project state save, and push history for undoable actions.

`SpeakersTab::refresh()`:

1. Clears local speaker/transcript state under `m_updating`.
2. Resolves the selected clip and active transcript path.
3. Clears avatar caches when the transcript/clip changes.
4. Loads the transcript JSON.
5. Rebuilds the speaker table and selected speaker panel.
6. Queues the FaceDetections paths panel refresh.

Speaker profile, reference, tracking, AI cleanup, and FaceDetections assignment
actions mutate the transcript document and/or sidecar artifacts, then schedule
save/history and refresh the affected UI.

## FaceDetections Generation and Assignment Flow

FaceDetections actions are editable only for mutable derived cuts. The selected clip
and active transcript are the input boundary.

Generation/processing flow:

1. UI action starts from `SpeakersTab`.
2. Runtime helpers render/decode frames using preview media paths and the Vulkan
   offscreen renderer where applicable.
3. Detections/tracks are converted into continuity roots with frame, normalized
   location, box size, confidence, detector mode, and scan range metadata.
4. Dense generated artifacts are written to the media-adjacent clip sidecar
   directory.
5. Raw continuity roots are saved to the raw FaceDetections artifact by clip id,
   referencing the media-adjacent artifacts.
6. Processed continuity roots are derived and saved to the processed artifact by
   clip id.
7. Speaker assignment metadata in the transcript may reference stream/track
   identities.
8. UI refresh reads streams through `continuityStreamsForRoot()` so it can use
   processed streams, raw-derived streams, or legacy transcript fallback.

Deletion removes the clip entry from both raw and processed artifacts and also
removes the clip's media-adjacent generated artifact directory.

## Current Guardrails

Follow these rules when changing dataflow-sensitive code:

1. Do not serialize project state or history on the playback hot path.
2. Keep dense/generated artifacts out of `state.json` and history snapshots.
3. Use `m_loadingState`, `m_restoringHistory`, and tab-level `m_updating` guards
   when applying state or rebuilding UI.
4. Use `QSignalBlocker` when restoring widget values or repopulating tables that
   have mutation handlers connected.
5. Preserve selection across refresh when the selected object still exists.
6. Collapse bursty refresh work with a timer instead of running repeated table or
   JSON rebuilds inline.
7. Write files through `QSaveFile` or existing `TranscriptEngine` helpers.
8. For undoable user edits, call `scheduleSaveState()` and `pushHistorySnapshot()`
   after the mutation is complete.
9. For non-undoable runtime/generated artifacts, update sidecars and schedule a
   project save only when project-visible metadata changed.
10. Keep render/preview/cache sync explicit after timeline mutations.

### Review Checks

These are the questions a reviewer should be able to answer from the patch:

1. If a new field is persisted, does it clearly belong in `state.json`,
   `history.json`, transcript JSON, or a sidecar?
2. If transcript JSON is mutated, is the history behavior intentional and
   stated by the call site?
3. If a widget rebuild path was added, does it guard against signal feedback and
   selection loss?
4. If a timer or debounce was added, is the trigger source and owner clear?
5. If a new path runs during playback or playhead-follow, does it avoid blocking
   I/O and broad UI churn?

## Known Gaps

These are architectural constraints, not bugs by themselves:

- There is no central reducer or immutable store. Many UI handlers still mutate
  timeline clips, transcript JSON, sidecar artifacts, and widgets directly.
  Why this matters: ordering bugs are easier to introduce because mutation,
  persistence, and refresh are often adjacent.
- Refresh methods are not pure render functions.
  Why this matters: refresh-trigger loops and hidden document reload costs are
  harder to see during review.
- Persistence, transcript mutation, and UI refresh are still coupled in several
  tab action handlers.
  Why this matters: interaction paths can accidentally pick up file I/O or broad
  repaint costs.
- `history.json` is count-bounded but not byte-budgeted.
  Why this matters: a small number of large snapshots can still grow project
  storage unexpectedly.
- Worker result schemas are normalized locally by subsystem, not by a single
  shared reducer boundary.
  Why this matters: result ingestion rules are duplicated and can drift.

When adding new features, prefer moving code toward explicit phases:

`read inputs -> mutate domain state/artifact -> sync runtime caches -> schedule persistence/history -> render UI`

## Change Checklist

Before merging a dataflow-sensitive change, check:

- did I introduce a new source of truth instead of reusing existing canonical
  state?
- did I put generated/heavy data into project state or history by accident?
- did I define whether the action is undoable and where the history snapshot
  happens?
- did I add file I/O to a refresh, selection-change, or playback-driven path?
- did I protect widget rebuilds from signal feedback?
- did I keep preview/audio/cache synchronization explicit after domain mutation?

## Regression Checks

Relevant tests and smoke checks live under `tests/` and target the pieces most
likely to regress dataflow:

- `tests/test_timeline_cache.cpp`
- `tests/test_async_decoder.cpp`
- `tests/test_playback_policy.cpp`
- `tests/test_transform_skip_aware_timing.cpp`
- `tests/test_transcript_logic.cpp`
- `tests/test_transcript_tab_follow.cpp`
- `tests/test_waveform_service.cpp`
- `tests/test_memory_budget.cpp`

Manual checks after dataflow changes:

- startup reaches an interactive first frame
- inspector refreshes do not recurse or peg CPU
- undo/redo restores selected timeline state and refreshes tabs
- playback does not trigger repeated state writes
- FaceDetections generation/deletion updates sidecars without bloating project state
- transcript/speaker edits persist to the active transcript and remain visible
  after project reload
