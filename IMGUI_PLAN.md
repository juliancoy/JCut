# Dear ImGui UI Parity Plan

## Goal

Provide a Dear ImGui implementation of JCut that can eventually replace the Qt shell without
forking editor behavior or project data.

The Qt and ImGui frontends must reuse the same editor/runtime code, project directory, active
project marker, `state.json`, and `history.json`. UI frameworks should own presentation and input;
editing rules, serialization, rendering, export, and history belong in shared services.

## Current Status (2026-07-22)

The ImGui editor is now a substantially more capable migration checkpoint, but it is **not yet
fully on par with the Qt editor**.

The core editing loop, safe project interchange, undo/redo, a broad set of inspector bindings,
preview, and standalone export are implemented. Advanced editor workflows and strict Qt-free
linkage of the final `jcut_imgui` executable remain incomplete.

The ImGui shell source now talks to preview audio and external Vulkan frames only through neutral
public interfaces. The audio implementation and rich preview producer remain Qt-backed, while the
external Vulkan importer itself has moved to a shared Qt-free core used by both frontends. This
keeps the current audio behavior and zero-copy preview path while the remaining implementation
cores are extracted. The audio facade also orchestrates synchronization, source polling, status,
and asynchronous warmup.

The built `jcut_imgui` executable and its CMake link interfaces have now dropped Qt Widgets, Qt
Network, and Qt Concurrent. The decoded-frame payload extraction also removes the private Qt Gui
target from the ImGui runtime interfaces. These are useful target-boundary milestones, not
completion of the Qt removal or evidence of full UI parity: the executable still links Qt6 Gui
and Core directly, with Qt6 DBus present transitively, and the strict all-Qt linkage check still
fails.

The complete unfiltered repository build now passes with `./build.sh`. Two dependency regressions
introduced while splitting the ImGui runtime boundary were corrected before accepting that
checkpoint: the offscreen face-detection executable now links its direct `Qt6::Network`
dependency for `QLocalSocket`, and `startup_project_state.cpp` lives with the widget-facing
`ProjectManager` implementation instead of creating a static-library dependency cycle through
`jcut_runtime_support`. The optional Qt CorePrivate/GuiPrivate discovery warnings remain benign.
Passing this build gate establishes build integrity only; it does not establish UI, preview,
audio, or export parity.

## Reuse Strategy

The implementation deliberately extends existing code instead of creating ImGui-specific
business logic:

- `EditorDocumentCore` is the neutral document exchanged by the shell, runtime, project adapter,
  preview, and export paths.
- `EditorRuntime` owns editor commands, selection, transport, undo/redo, and history transaction
  behavior used by the ImGui controls.
- The legacy Qt state bridge preserves fields the neutral model does not yet understand and
  reuses existing render timeline and effect-preset conversion logic.
- ImGui project I/O operates directly on the Qt project store and history formats.
- Standalone export reuses the neutral document/render contracts and FFmpeg backend.
- The existing Qt editor remains buildable while boundaries are extracted incrementally.

Duplicating command semantics or serialization rules in `jcut_imgui_main.cpp` is considered a
migration bug.

## Implemented

### Neutral document and runtime

- Persistent media, track, and clip IDs.
- Media kind, source timing, clip timing, track assignment, and render ordering.
- Persisted audio-stream presence metadata for media-bin items and clips, with extension fallback
  for legacy unknown files.
- Track visual/audio flags, gain, mute, and solo state.
- Generated mask-child track identity (`generatedChildTrack`, `parentClipId`, and `childClipId`)
  round-trips through core/legacy JSON and both Qt/render bridge directions. A shared neutral
  reconciler creates one bounded child lane per valid matte, keeps it adjacent to its source,
  repairs stale/duplicate/orphan rows, recovers foreign ordinary clips instead of discarding them,
  and preserves the child lane's independent height, visual mode, and grading-preview state.
- Clip audio enablement, gain, pan, mute, and solo state.
- Transform, grading, opacity, and title state with keyframes.
- Effect, mask, and correction state.
- Clip ownership metadata (`clipRole` and `linkedSourceClipId`) round-trips through neutral JSON
  and the Qt/render bridges; unknown future roles remain opaque instead of being normalized away.
  Mask-matte children normalize their source/proxy, media, timing, playback-rate, and transform
  cache fields from the authoritative parent while retaining child-owned visual treatment.
- Transcript-overlay configuration.
- Render-sync markers and export ranges.
- Export request settings and transport state.
- Safe clip move, trim, single/razor-group split, delete, insert, selection, and nudge commands.
- Additive/toggle multi-selection plus reusable copy, cut, paste-at-playhead, duplicate, and
  select-all commands. Clipboard operations preserve relative track/frame layout and remap copied
  render-sync markers to fresh persistent clip IDs. Mask-matte ownership is treated as an
  aggregate: copying either a source or one of its children includes the source and all owned
  siblings, paste remaps their relationships, and destructive parent operations cascade without
  leaving orphaned children.
- Atomic selected-group delete, move, and nudge commands preserve relative clip layout, validate
  locked/out-of-range groups before mutation, and shift owned render-sync markers consistently.
  Parent move, group move, nudge, trim, resize, and playback-rate operations carry owned mask
  children as one timing aggregate and renormalize their caches; direct child timing edits are
  rejected instead of allowing parent/child drift.
- Clip lock/unlock is an undoable neutral command. Locked clips reject structural split, trim,
  move, resize, delete, and playback-rate edits at the runtime boundary; the shared playback-rate
  command applies the Qt duration/ripple policy and moves render-sync markers with rippled clips.
- Stable-ID track reordering preserves clip assignments and is independently undoable.
- Track-wide consecutive-clip crossfade state is represented neutrally by the Qt-compatible
  per-clip 48 kHz edge-fade length and round-trips through core/legacy JSON and both Qt/render
  bridge directions. The shared runtime command applies the Qt ordering, lock, duration,
  optional sample-domain overlap, and visual opacity-ramp policy as one undoable edit; generated
  child lanes cannot be targeted independently.
- Reusable media-lane conflict resolution routes timeline insertions without duplicating the Qt
  visual-versus-audio overlap policy in the shell. Generated child lanes are never returned as
  ordinary media targets, and indexed track creation supports the Qt drop-on-child-row behavior
  without treating the derived row as a reorder target.
- Atomic title-asset creation reuses that conflict policy and the existing neutral title/render
  path. It applies the Qt 90-frame visual-only defaults, creates a frame-zero title keyframe and
  stable persistent clip ID, selects the result, leaves the media bin untouched, always finds or
  creates the exact unnumbered `Titles` lane first, then reuses a free `Titles*` lane or creates
  the next numbered lane when the canonical lane is occupied.
- Channel-scoped grading, opacity, and transform keyframe removal is undoable; existing upsert
  commands are reused for Linear/Hold interpolation changes.
- Render-sync markers allow one mapping decision per canonical owner persistent clip ID and frame.
  Mask-child decisions canonicalize to the linked source, changing the operation updates that
  marker, counts follow the Qt `1..120` policy, and legacy duplicates load deterministically with
  the last value winning. Row edits and exact removal remain atomic and undoable instead of using
  broad clip/all-marker deletion.
- Atomic, undoable project-media removal rejects entries still referenced by timeline clips; it
  never cascades clip deletion. This matches the Qt shell's lack of a media-bin cascade policy.
- Undo/redo with deterministic snapshots and coalesced transactions for continuous controls and
  timeline drags.
- Redo invalidation after a new edit and separation of navigation-only state from edit history.

### Project and history interchange

- Loads the active Qt project through the same project root and active-project marker.
- Reads and writes the Qt `state.json` and `history.json` files.
- Preserves Qt-only nested clip/track fields while updating neutral fields.
- Preserves both legacy full-snapshot histories and snapshot-delta histories.
- Branches history at the active entry instead of retaining stale redo entries.
- Recovers malformed state from a valid snapshot-delta history.
- Rejects malformed or unsupported history before changing project files.
- Honors configured history entry and size limits while preserving the active snapshot.
- Rejects stale concurrent sessions instead of overwriting newer state.
- Serializes cross-process saves and uses atomic replacement for state/history commits.
- Rolls history back if the paired state commit fails.
- Validates a project before updating the active-project marker.
- Creates collision-safe normalized project directories, clones source history for Save As, and
  renames active projects without rewriting their state/history payloads.
- Coordinates managed-session saves with lifecycle mutations, rejects stale sessions after a
  rename, and rejects symlinked project paths that escape the active store.
- Activates and loads project switches as one shell operation, restoring the previous marker if
  the target cannot be loaded.
- Deterministic lifecycle failure injection covers New, Save As, Rename, and Activate marker
  commits, post-commit session-load rollback, rename rollback/recovery, and incomplete-project
  cleanup. Cleanup failures are retained safely and reported instead of being silently discarded.
- Neutral backend history listing reconstructs legacy and snapshot-delta entries with active index,
  playhead frame, and clip-count summaries. Atomic entry activation updates the paired state/history
  files without truncating redo entries, rejects stale or escaped sessions, and returns a refreshed
  managed session. The History tab lists those summaries and restores a selected saved snapshot
  behind the shared dirty-state guard. Saves, Save As, and Rename invalidate the shell's cached
  history rows so a newly adopted project can never act on indices displayed for the prior one.
- Persists ImGui media-bin entries through the legacy `mediaItems` extension. The Qt shell keeps
  that extension opaquely through project load, save, Save As, autosave, and history snapshots,
  so switching shells no longer discards the ImGui collection even though Qt still has no
  first-class media-bin UI for it.

### ImGui shell

- Real File/Edit/Playback menus and text-input-aware shortcuts for new project, save, Save As,
  reload, undo, redo, multi-select clipboard operations, split, delete, duplicate, nudge,
  play/pause, and frame stepping. New, Save As, and Rename reuse one lifecycle modal and shared
  project-store operations.
- Media browsing, import, guarded project-bin removal, track creation, clip insertion, and
  selection. Project-media removal is exposed through an item context menu and reuses the neutral
  runtime command.
- Timeline seek, additive/toggle select, single/group move, start/end trim, single/group razor
  split, boundary snapping with visible feedback, batch delete, group nudge, track reorder controls,
  and playhead behavior. The clip context menu exposes clipboard, nudge, split, playback-rate,
  lock/unlock, Scale-to-Fill, atomic grading reset, and counted render-sync actions through shared
  commands, and locked clips no longer present active drag/trim gestures. Colored render-sync
  markers are drawn on their canonical source clip, expose action/count tooltips, and open the same
  undoable actions when right-clicked.
- Project-bin and filesystem media drag/drop uses copied stable payloads, probed source durations,
  gap/empty-timeline track creation, and same-lane overlap routing. Track creation, reorder, and
  insertion are coalesced into one undo transaction. Numbered image-sequence directories reuse a
  Qt-free detector/natural frame listing across Qt, ImGui, probing, preview, and standalone export;
  they import as 30-fps visual clips whose duration is the detected frame count, while ordinary
  directories retain navigation behavior.
- Generated mask-child lanes have an indented/subdued timeline treatment and read-only derived
  identity. Ordinary media dropped on a child row creates a normal lane at that row boundary;
  ordinary clip moves and conflict routing reject child lanes. Matte clips do not advertise direct
  move/trim handles, and ordinary track reorder controls skip generated rows while source-track
  reorder/delete continues to carry the complete child hierarchy through the neutral runtime.
- Bound Grade, Opacity, Effects, Masks, Corrections, Titles, Sync, Transform, Transcript Overlay,
  Audio, Output, and Projects controls for the neutral state currently represented. Tracks expose
  neutral label/height editing, stable-ID reorder, tri-state visual mode, independent grading
  preview, and audio enable/gain/mute/solo through focused shared runtime commands. Generated rows
  keep selection, bounded height, visual mode, and grading preview editable while derived label,
  order, and audio controls remain unavailable.
- Tracks expose the Qt-style consecutive-clip crossfade duration and optional move-to-overlap
  controls through the neutral runtime command. Preview-audio timeline identity includes the
  per-clip fade, so a no-move crossfade cannot leave the reused Qt mixer on stale state.
- Effects consumes the shared Qt-free catalog of all 35 canonical preset IDs instead of keeping a
  partial shell-local list. The represented copies/radius, speed, scale/intensity, and alternate
  direction fields use preset-aware labels and ranges; neutral command normalization now matches
  the Qt `1..96` (`1..512` for progressive edge), `-8..8`, and `0.1..8` persistence bounds, so
  reverse and stationary effect motion survive ImGui edits.
- Mask-only controls use a focused neutral command, so toggling or tuning a mask does not normalize
  or overwrite unknown forward-compatible effect preset fields.
- Transcript Overlay exposes all neutral-backed placement, typography, shadow, and text,
  background, and word-highlight color fields through the shared undoable runtime command.
- The Transcript tab now reuses a Qt-free cut-session/catalog service for source/audio-stream
  resolution, editable and numbered-cut discovery, custom labels, persisted catalog/custom-cut fallback,
  and unknown-field-preserving row projection. Its cached, clipped table shows timing, render
  ranges, speakers, text, edit flags, gaps, skipped words, and optional outside-cut rows without
  reparsing every frame. Cut selection is stored on the neutral clip so the rich render bridge and
  project history consume the same version, while the selected-cut legacy field remains mirrored
  for Qt compatibility. Transcript-content editing remains read-only in this tranche; cut
  selection and its project/history state are writable and undoable.
- The Qt `TranscriptTab` also delegates asynchronous read-only row construction, overlap handling,
  gaps, outside-cut projection, speaker labels, and edit flags to `TranscriptDocumentCore`. Its thin
  adapter maps projected active-word locations back to stable in-memory Qt word IDs, so refreshes
  and deletion do not retarget selection, follow state, or subsequent edits.
- Grade, Opacity, and Transform share a neutral-backed keyframe draft editor for every represented
  frame/value field, row load/seek, Linear/Hold changes, and scoped removal. Moving a keyframe
  coalesces remove-old plus upsert-new into one runtime undo step; legacy serialization carries
  future/other Qt-only material and mask fields to the reframed row when the key count is stable.
  All Qt-known tonal and curve grading fields are represented neutrally. The ImGui Grade draft
  binds all nine Lift/Gamma/Gain RGB values, and a shared full-grading evaluator supplies the
  interpolated or held grade, curves, and opacity used to seed New At Playhead. Grading reset
  returns all known fields to canonical defaults while preserving unknown future fields. A shared
  Qt-free grading-curve helper owns sanitization and three-point tone/curve mapping for both
  shells. ImGui exposes bounded RGB/Luma point tables, endpoint locking, add/remove/reset,
  smoothing, and three-point lock controls; locked tone edits regenerate the stored RGB curves.
  The same helper now owns Qt-compatible curve sampling, byte LUT construction, channel/Luma
  composition, deterministic bounded simplification, and Normalize Curves for both frontends;
  non-finite text/JSON input follows the prior Qt bounds instead of reaching an unsafe conversion.
- Title and track text editing uses Dear ImGui's dynamically sized `std::string` adapter, avoiding
  fixed-byte truncation and partial UTF-8 writes during otherwise unrelated style/property edits.
- The Titles tab creates a ready-to-edit title at the playhead through the atomic neutral command,
  using the same dedicated `Titles`/`Titles N` lane-routing policy as the Qt timeline action;
  command failures are surfaced through the shared shell status message.
- Keyframe frame cells seek through the neutral transport command. Sync rows support direct
  seeking, frame/count/action editing coalesced into one undo step, and individually undoable
  marker removal in addition to add/clear actions. The clip context menu reuses those commands at
  the playhead with Qt-matching `1..120` count bounds and same-action count defaults. Timeline
  markers are hit-testable and reuse the same context actions; popup and modal identity is guarded
  by both persistent clip ID and document generation across reload/project switches.
- Inspector controls use history transactions so one slider drag creates one undo step.
- Existing-project switching with validation, dirty-state protection, atomic marker/session
  adoption, and safe reload behavior.
- Export job status, progress, cancellation, and result reporting.
- Real histogram/scope input from CPU preview frames and live render/handoff telemetry.
- Movable and resizable panels after the initial layout, persisted in `imgui_layout.ini`.
- Persisted ImGui preferences and output-path drafts.
- Dirty-state protection for reload, project switching, and window close.
- Save/Discard/Cancel confirmation on a dirty window close.

### Preview and export hardening

- Preview work is asynchronous and the UI remains responsive while a frame is rendered.
- Borrowed zero-copy Vulkan frames are not reused until the UI acknowledges upload completion.
- Failed/stale previews clear their texture rather than displaying the previous frame as current.
- Control-server preview requests are transferred to the UI thread safely.
- Standalone FFmpeg export supports encoded MP4, MOV, and MKV video plus image-sequence output.
  Format selection explicitly controls the muxer even when the filename extension disagrees.
  WebM is routed correctly and uses portable libvpx VP8/VP9 encoders when the bundled FFmpeg build
  provides one; otherwise export fails clearly instead of selecting an unusable device encoder or
  silently writing MP4.
- The standalone FFmpeg boundary also performs import-time and legacy document-load stream probes,
  so silent visual media is not optimistically scheduled as audio. Stream presence remains
  independent of the clip's user-controlled Audio toggle.
- Export settings, playback speed, progress, cancellation, and output paths use the neutral render
  contract.
- The standalone ImGui exporter currently emits video/image streams only and has no audio
  stream/mixer. This is a confirmed parity blocker: preview audio is functional through the Qt
  adapter, and the Qt export mixer applies per-clip edge fades, but an ImGui standalone export
  cannot yet contain audio at all.
- The first Qt-free standalone-audio extraction is complete: `standalone_audio_mixer` owns neutral
  48 kHz stereo timeline/source mapping, render-sync duplicate/skip adjustment, clip/track
  enablement, gain/mute/solo policy, clipping, and per-clip edge fades. Synthetic Qt-free tests
  cover these policies. Media decoding, pitch-preserving speed conversion, audio encoding, and
  mux interleaving still must be connected before the preceding blocker is closed.

### Neutral preview/audio boundaries

- `OffscreenVulkanFrame` now lives in a small Vulkan/core header rather than the Qt-heavy render
  internals header.
- `ImGuiAudioRuntime` is a Qt-free PIMPL interface used by the shell; its current adapter reuses
  `AudioEngine`, shared render-timeline conversion, and the existing playback-warp policy.
- Audio timeline changes are installed as one mixer-visible state update, explicit/relative audio
  sources are resolved before conversion, and visual-only edits do not change the audio timeline
  signature or trigger reconstruction; throttled filesystem identity polling continues
  independently.
- Audio warmup runs asynchronously, exposes an explicit buffering state, and holds the neutral
  transport while decode/time-stretch work is still pending. A timed-out warm interval is retried
  without advancing transport; playback starts only after audio is ready, after a confirmed
  terminal source failure (allowing the rest of the timeline to continue), or when the output is
  unavailable and the neutral transport must provide the clock.
- Filesystem identity checks are throttled to 250 ms and cover source files, explicit audio files,
  and derived same-stem WAV sidecars using existence, size, and modification time. Changes
  invalidate versioned decode/time-stretch caches and refresh stream topology when media at a
  stable path is replaced. Stale decode, RubberBand progress/failure, sidecar-write, and mixer work
  cannot repopulate the new generation or refill cleared buffers.
- Ring-buffer reset now rendezvouses safely with the real-time reader, avoiding a seek/project-
  switch race that could otherwise expose overwritten samples.
- `FramePayloadCore` owns neutral frame identity, dimensions, crop metadata, opaque CPU/GPU
  payload bookkeeping, AVFrame clone/free lifetime, and the existing CPU/GPU residency estimates.
  `FrameHandle` remains the Qt adapter seen by current consumers, but no longer dereferences QRhi
  private types in the ImGui chain. Its production-unused QRhi texture factory/upload methods stay
  source-compatible in a Qt editor/test-only adapter, preserving the legacy Qt API while keeping
  private QRhi out of the ImGui targets. This eliminates `Qt6::GuiPrivate` from
  `jcut_preview_decode_runtime` and `jcut_runtime_support` without changing decode, cache, rich
  preview, or audio behavior.
- `VulkanFrameImporter` is a Qt-free PIMPL interface used by the shell and now owns the shared
  Qt-free opaque-FD import core directly. `VulkanDetectorFrameHandoff` delegates its external-frame
  import/copy path to that same core, preserving the existing local sampled-image copy, layout
  restoration, borrowed-frame lifetime, and caller-owned ready-semaphore behavior without
  duplicating the implementation. The shell's `PreviewRenderResult.image` path retains CPU
  fallback.
- `jcut_preview_decode_runtime` is an AUTOMOC object target containing the existing
  `AsyncDecoder`, `MemoryBudget`, and `TimelineCache` implementation cluster. Its generated MOC
  object is separate from the `ProjectManager` and `TranscriptTab` MOCs that remain in
  `editor_test_support`, so using preview decode/cache no longer pulls Qt widget implementations
  into `jcut_imgui`.
- `jcut_runtime_support` contains the non-widget legacy helpers still needed by preview and audio.
  Widget dialogs, project UI, and transcript-tab implementations remain in
  `editor_test_support`; the ImGui CMake chain no longer names Qt Widgets, Qt Network, Qt
  Concurrent, or Qt GuiPrivate, and a generated link-interface manifest test guards that
  boundary. Image-sequence
  batch loading retains bounded parallelism through a Qt Core thread pool instead of
  `QtConcurrent::blockingMapped`; fire-and-forget waveform/continuity jobs use the same Core pool,
  while the future-based transcript save controller remains with widget-facing test/editor
  support.
- A standalone neutral-boundary executable includes the public editor, preview, audio, and frame-
  import contracts without instantiating the Qt-backed adapters; it links only `jcut_editor_core`
  and Vulkan and passes the strict no-Qt linkage checker.
- A focused importer-boundary executable instantiates the real `VulkanFrameImporter`, exercises
  inert/invalid lifecycle behavior, and is guarded by the strict no-Qt linkage checker.
- The linkage checker accepts a focused Qt component, and the final executable passes the focused
  no-Widgets, no-Network, and no-Concurrent checks.

## Current Target Boundaries

| Target | Qt-free | Responsibility |
| --- | --- | --- |
| `jcut_frame_payload_core` | Yes | Neutral decoded-frame identity, size/crop metadata, opaque payload ownership/bookkeeping, AVFrame lifetime, and memory accounting. |
| `jcut_frame_handle_qrhi_adapter` | No | Legacy `FrameHandle` QRhi factory/upload compatibility, linked only by Qt editor/test targets and excluded from the ImGui runtime chain. |
| `jcut_transcript_document_core` | Yes | Unknown-field-preserving WhisperX-style parsing and row projection reused by both ImGui and Qt read-only tables, plus shared source/audio-stream resolution, editable/version catalog discovery, labels, active-cut fallback, file stamps, and read-only cut sessions. |
| `jcut_media_path_core` | Yes | Shared image-sequence-directory detection and naturally ordered frame listing. |
| `jcut_editor_core` | Yes | Neutral document JSON, project I/O, runtime commands/history, render-contract JSON, and control server. |
| `jcut_vulkan_frame_import_core` | Yes | Shared opaque-FD external-memory import, local sampled-image copy, layout restoration, and Vulkan resource ownership. |
| `jcut_imgui_vulkan_import` | Yes | Dear ImGui's neutral PIMPL facade over the shared Vulkan import core. |
| `jcut_imgui_standalone_runtime` | Yes | Standalone timeline/image-sequence rendering, import/load media stream probing, and FFmpeg export. |
| `jcut_preview_decode_runtime` | No | Existing QObject-based asynchronous decoder, memory budget, and timeline cache, isolated in its own AUTOMOC object target. |
| `jcut_runtime_support` | No | Non-widget legacy renderer/decoder, audio, transcript, and timing helpers; exposes Qt Gui/Core but not Widgets, Network, or Concurrent. |
| `editor_test_support` | No | Widget-facing dialogs, project UI, and transcript-tab support retained for the Qt editor and tests; excluded from the ImGui dependency chain. |
| `jcut_editor_runtime` | No | Existing Qt-backed render bridge, Vulkan renderer, overlays, titles, and audio engine. |
| `jcut_imgui_qt_adapters` | No | Neutral ImGui audio facade over the existing Qt-backed `AudioEngine`; external-frame import has moved to its Qt-free target. |
| `jcut_imgui_runtime` | No | Current preview implementation; links both standalone and Qt-backed runtime targets. |
| `jcut_imgui` | No | X11/Vulkan/ImGui shell with neutral public source boundaries; no Qt Widgets linkage, but still inherits other Qt libraries through adapter and preview implementations. |
| `editor` (`jcut`) | No | Existing Qt frontend, kept working throughout migration. |

The current dependency chain is:

```text
jcut_imgui
  -> jcut_imgui_runtime
     -> jcut_imgui_standalone_runtime -> jcut_editor_core + jcut_media_path_core
     -> jcut_imgui_vulkan_import -> jcut_vulkan_frame_import_core
     -> jcut_imgui_qt_adapters (audio) -> jcut_editor_runtime
     -> jcut_editor_runtime
        -> jcut_runtime_support
           -> jcut_frame_payload_core
           -> jcut_media_path_core
           -> jcut_preview_decode_runtime -> jcut_frame_payload_core + Qt6 Core/Gui
           -> remaining non-widget legacy support -> Qt6 Gui/Core
```

Accordingly, the claim that the final ImGui binary is Qt-free is not yet true. In the current Linux
build, `readelf` reports direct dependencies on Qt6 Gui and Core; `ldd` also reports Qt6 DBus
transitively. Qt6 Widgets, Network, and Concurrent are absent from both the generated ImGui link
interfaces and the binary, while Qt6 GuiPrivate is absent from the generated runtime interfaces;
focused binary checks and the CMake-interface regression check guard those reductions.

## UI Parity Matrix

| Area | Current ImGui coverage | Work still required for Qt parity |
| --- | --- | --- |
| Projects | Safe new/save/Save As/rename/load/reload and existing-project switching, with dirty-state guards, shared-store collision handling, stale-session/path validation, and transactional failure-injection coverage | Qt-native dialog polish and exact cross-shell UX equivalence |
| Project history | Preserved disk history, runtime undo/redo/depth, and a saved-snapshot browser with atomic restore and redo preservation | Rich history metadata and cross-shell command equivalence |
| Media | Browse, filter, import, safe project-bin removal, cross-shell preservation, add track, insert clip, project/filesystem drag/drop, and naturally ordered image-sequence-directory import/drop/render | Proxy management and complete metadata/actions |
| Transport | Seek, play/pause, speed, frame step | Exact Qt policies and all transport preferences |
| Timeline | Additive/toggle selection, parent-authoritative source/mask timing normalization, rejection of direct child timing edits, relationship-safe aggregate move/nudge/trim/resize/rate/split/copy/delete with marker movement, generated-child reconciliation/presentation/drop protection, visible snapping, batch delete/nudge, copy/cut/paste, duplicate, clip rate/lock, Scale-to-Fill, Qt-complete grading reset, visible/right-clickable counted render-sync markers, stable-ID source-track reorder with child grouping, media drag/drop with gap creation/conflict routing, export ranges, and core context actions | Group-drag polish and complete Qt context-action coverage |
| Tracks | Neutral label/height editing, stable-ID reorder, tri-state visual mode, independent grading preview, audio enable/gain/mute/solo, and consecutive-clip crossfade duration/overlap controls are bound; generated rows preserve editable visual/grade/height state while derived identity/order/audio remain guarded | Media-presence enablement and exact sidebar gestures |
| Preview | Real asynchronous Vulkan/CPU preview with zoom/pan | Qt-free render path and exact rich-composition parity |
| Grade | Base values plus frame/brightness/contrast/saturation/opacity/interpolation and all nine Lift/Gamma/Gain RGB values are editable; row load/seek, creation/removal, neutral full-grade evaluation for New At Playhead, bounded RGB/Luma curve-point editing, smoothing/three-point lock with shared tone-to-curve synchronization, shared Qt-compatible curve normalization, neutral storage of all Qt-known curve fields, and atomic Qt-complete grade/opacity reset are bound | Qt-style graphical curve/histogram direct manipulation and auto-oppose |
| Opacity | Value, fades, and full neutral keyframe frame/value/interpolation editing with row load/seek, creation, and removal are bound | Direct preview manipulation and exact Qt workflow polish |
| Effects and masks | All 35 canonical presets and every neutral-backed common parameter are bound with contextual labels/ranges; basic mask state is bound | Promote Qt-only speech-sync, difference/echo, and tiling parameters to the neutral model; drawing/manipulation and exact preview behavior |
| Corrections | Enablement and clearing are bound | Polygon drawing, ranges, point editing, overlay workflow |
| Titles | Atomic title-asset creation plus neutral text, placement, font, emphasis, opacity, color, interpolation, row load/seek, and removal are bound | Direct-preview manipulation and Qt-only shadow/material/extrusion/window workflows |
| Sync and transform | Sync add/clear/individual removal, row seeking, frame/count/action editing, canonical source ownership, and visible tooltip/right-click timeline markers are bound; transform exposes full neutral frame/title/translation/rotation/scale/interpolation editing with row load/seek, creation, removal, and an undoable Scale-to-Fill context action using shared Qt-free aspect math | Direct transform preview manipulation and exact Qt interaction polish |
| Transcript | Every neutral-backed overlay field is bound; ImGui uses the Qt-free cut-session/catalog core for cached path/version loading and a populated clipped read-only table, including gaps, edit flags, speaker labels, skipped/outside-cut rows, persisted cut selection, and rich-preview propagation. Qt read-only row construction now reuses `TranscriptDocumentCore` and preserves its stable in-memory word IDs across refresh/deletion | Adopt shared catalog/version selection and mutation/save/restore/history services in the Qt tab, then add ImGui transcript-content editing and context actions |
| Speakers/faces | Status/scaffolding only | Roster, assignment, continuity, mining, detection, titles, diagnostics |
| Audio | Clip state plus Qt-backed preview audio/status; track mix controls are covered in Tracks | Neutral audio service, full routing, dynamics, mixing, isolation, devices |
| Export | MP4/MOV/MKV video or image sequence, explicit muxer routing, WebM selection with a clear libvpx requirement, settings, progress, cancel | Add standalone audio mixing/encoding first; bundle/require a portable VPx encoder for universal WebM output; then complete Qt option/output-equivalence coverage |
| Jobs | Current export job is functional | General processing job creation, diagnostics, and results |
| Scopes/pipeline | Live preview histogram and render telemetry | Full Qt scopes, pipeline graph, controls, and diagnostics |
| AI/access | Scaffolding only | Shared AI, entitlement, model, chat, and refresh services |
| Preferences/system | Layout/font and a small settings subset | Decoder controls, benchmarks, hardware/device settings, full persistence |

## Remaining Implementation Order

1. **Remove Qt from the ImGui preview/audio dependency chain.**
   - Completed: move the neutral Vulkan frame type out of `render_internal.h`.
   - Completed: make the ImGui shell consume neutral audio and external-frame import interfaces.
   - Completed: preserve the existing `AudioEngine` behind its neutral adapter and preserve the
     zero-copy importer behind a neutral facade while extraction continues.
   - Completed: isolate preview decode/cache QObjects and their MOCs in
     `jcut_preview_decode_runtime`; the final executable now passes the focused no-Qt-Widgets
     linkage check.
   - Completed: split non-widget runtime helpers into `jcut_runtime_support` and keep widget-facing
     test/editor support out of the ImGui chain, so its CMake link interface exposes neither Qt
     Widgets nor Qt Network.
   - Completed: replace image-sequence `QtConcurrent::blockingMapped` with bounded Qt Core pool
     workers and remove Qt Concurrent from the ImGui binary and target interfaces.
   - Completed: extract opaque-FD external-frame import/copy into
     `jcut_vulkan_frame_import_core`, make both the ImGui facade and legacy handoff reuse it, and
     split the importer out of `jcut_imgui_qt_adapters`. This is a real boundary reduction, but
     the rich preview producer and audio adapter still keep Qt Gui/Core in the final executable.
   - Completed: extract decoded-frame ownership, normalized crop metadata, and residency
     accounting into `jcut_frame_payload_core`; retain `FrameHandle` as a Qt adapter and remove
     its unused private-QRhi creation/upload implementation from the ImGui target chain (while
     retaining it for Qt editor/test compatibility), so the ImGui runtime targets no longer expose
     Qt GuiPrivate.
   - Remove the remaining Qt6 Gui and Core dependencies, including the transitive Qt6 DBus
     dependency, without weakening preview composition or audio behavior.
   - Extract a portable audio implementation core from `AudioEngine`, then switch the neutral
     facade away from the Qt adapter after parity tests pass.
   - Extract preview composition behind the existing Qt-free interfaces without dropping rich
     composition or zero-copy behavior.
   - Make `jcut_imgui_runtime` depend only on Qt-free targets.
   - Require `tests/check_imgui_binary_no_qt.py` to pass.

2. **Adopt the shared runtime from the Qt shell.**
   - Route equivalent Qt editing actions through `EditorRuntime` commands.
   - Add command-script tests proving Qt and ImGui produce equivalent snapshots.

3. **Finish high-frequency editing workflows.**
   - Completed: neutral multi-selection and clipboard commands, atomic group move/delete/nudge,
     stable-ID track reorder, shell shortcuts/Edit menu, and core timeline context actions.
   - Completed: shared razor-group splitting, visible move/trim/drop snapping, and project/filesystem
     media drag/drop with empty/gap track creation and same-lane conflict routing.
   - Completed: shared image-sequence-directory detection, natural frame ordering, deterministic
     30-fps/frame-count probing, filesystem/project insertion, and standalone frame decoding.
   - Completed: neutral clip lock enforcement plus Qt-style playback-rate duration/ripple behavior,
     bound with nudge, rate, and lock/unlock actions in the ImGui clip context menu.
   - Completed: reuse the undoable neutral transform command for Scale to Fill Preview, share its
     aspect-fit multiplier with the Qt shell, restore Qt still-image size probing, and expose
     Qt-free still/video/image-sequence source dimensions through the standalone FFmpeg probe.
   - Completed: reuse neutral render-sync commands from the clip context menu with Qt count/default
     semantics, draw and hit-test canonical-owner timeline markers with the same context commands,
     and guard their modal/context identity across document reloads.
   - Completed: model every Qt-known grading tonal/curve field and add an atomic grading reset that
     restores those fields, neutral base grade/opacity, and frame-zero visual keys without
     disturbing unrelated or unknown future clip state.
   - Completed: preserve source/mask-matte ownership through parent-authoritative cache
     normalization, rejection of direct child timing edits, aggregate move/nudge/trim/resize/rate,
     copy/paste/split/delete, and relationship/render-sync marker remapping.
   - Completed: neutral generated-child track identity/reconciliation, JSON and Qt/render bridge
     round trips, derived-row presentation, indexed normal-lane drops, guarded child targets,
     source-track hierarchy reorder/delete, and independently editable child visual/grade/height
     state.
   - Completed: neutral consecutive-track crossfade command, per-clip fade persistence/bridging,
     ImGui duration/overlap controls, generated-lane guards, and preview-audio signature refresh.
   - Remaining group-drag polish and context actions.
   - Complete keyboard/focus behavior.

4. **Finish advanced inspector workflows.**
   - Completed: undoable per-row removal and Linear/Hold changes for grading, opacity, and
     transform keyframes.
   - Completed: keyframe/sync click-to-seek navigation plus undoable sync-marker
     frame/count/action editing and individual removal.
   - Completed: neutral title-keyframe text, placement, font, emphasis, opacity, color,
     interpolation, row load/seek, and removal editing.
   - Completed: atomic title-asset creation at the playhead with Qt defaults, stable IDs,
     exact canonical-`Titles` preference, conflict-safe numbered-lane routing, and no synthetic
     media item.
   - Completed: shared Qt-compatible curve sampling/LUT composition/simplification and Normalize
     Curves in both frontends, in addition to bounded point-table editing and shared three-point
     lock/smoothing behavior.
   - Grade Qt-style graphical curve/histogram direct manipulation and auto-oppose.
   - Mask/correction drawing and point manipulation.
   - Complete title direct manipulation, preview manipulation polish, and the remaining Qt-only
     effect parameters (speech sync, difference/echo, and tiling) and audio controls.

5. **Extract the remaining shared services.**
   - Completed: extract a read-only Qt-free transcript document/row projection core with
     unknown-field-preserving source JSON retention and focused strict-no-Qt coverage.
   - Completed: add a shared Qt-free cut-session/catalog adapter and wire ImGui cached
     path/version loading, active-cut persistence, rich-preview propagation, file-change
     invalidation, and clipped read-only rows. The Qt runtime registry takes precedence over a
     neutral persisted cut when the Qt user changes versions in-process.
   - Completed: move Qt `TranscriptTab` read-only row construction onto
     `TranscriptDocumentCore`, retaining stable Qt word IDs across asynchronous refreshes and
     deletion while reusing shared timing, gap, outside-cut, speaker, and edit-flag projection.
   - Move the Qt catalog/version-selection and mutation/save/restore/transcript-history paths onto
     shared services, then add the corresponding ImGui transcript-content editing workflows.
   - Speakers, face detection, continuity, AI, and access.
   - General processing jobs, decoder controls, diagnostics, and full preferences.

6. **Close preview/export equivalence.**
   - Add a Qt-free standalone audio mixer/encoder path to ImGui export. It must honor clip/track
     enablement, gain/pan/solo, source timing, playback rate, render-sync mapping, export ranges,
     and per-clip edge fades before standalone export can be considered functionally complete.
   - Compare rich composition, overlays, transcripts, grading, masks, effects, titles, audio, and
     export output from identical projects.

7. **Add shell and cross-shell parity automation.**
   - Complex project roundtrips without state/history loss.
   - Shortcut and interaction smoke tests.
   - Detection of visible but unwired controls.
   - Equivalent snapshots, preview frames, and exports after identical command sequences.

## Verification

Builds must use the repository wrapper:

```bash
./build.sh --target jcut_imgui
./build.sh --target editor
./build.sh --with-tests --target test_editor_runtime
./build.sh --with-tests --target test_imgui_project_history
./build.sh --with-tests --target test_imgui_standalone_export
./build.sh --with-tests --target test_imgui_standalone_render
./build.sh --with-tests --target test_imgui_neutral_boundary
./build.sh --with-tests --target test_grading_keyframes
./build.sh --with-tests --target test_media_drag_drop
./build.sh --with-tests --target test_imgui_vulkan_import_boundary
./build.sh --with-tests --target test_frame_payload_core
./build.sh --with-tests --target test_frame_handle
./build.sh --with-tests --target test_transcript_document_core
./build.sh --with-tests --target test_transcript_cut_session_core
./build.sh --with-tests --target test_transcript_tab_follow
./build.sh --with-tests --target test_audio_mix_policy
./build.sh --with-tests --target test_audio_time_stretch_cache
./build.sh --with-tests --target test_preview_geometry
./build.sh --with-tests --target test_media_color_passthrough
./build.sh --with-tests --target test_media_root_contract
```

The acceptance gate is the unfiltered wrapper command, not only selected targets:

```bash
./build.sh
```

This full build passed after restoring the direct `Qt6::Network` dependency of
`jcut_vulkan_facedetections_offscreen` and moving the `ProjectManager`-dependent startup source
out of the lower-level runtime-support archive.

Focused test results for this checkpoint:

- `test_editor_runtime`: 65 passed; focused coverage includes guarded undoable project-media removal, neutral
  razor splitting, media-lane conflict routing, atomic filesystem-drop insertion/undo coverage, and
  channel-scoped keyframe removal, atomic keyframe reframe/value edits, full neutral
  title/transcript field round trips, atomic title creation/default/render/undo coverage with
  media-bin isolation and ImGui source wiring, Qt-known grading reset with unknown-field
  preservation, canonical mask-owner sync normalization, parent-authoritative cache normalization,
  rejected direct child timing edits, relationship-safe aggregate move/nudge/trim/resize/rate,
  copy/cut/paste/delete/split, and marker movement/remapping, count bounds, scoped sync marker
  removal, clip lock enforcement, playback-rate ripple behavior, shared Scale-to-Fill aspect math
  and undo/redo behavior, plus neutral/Qt effect-preset catalog equivalence,
  reverse/stationary effect-speed bounds, and undoable grading curve sanitization, tonal clamping,
  locked RGB regeneration, unlocked curve preservation, generated-child JSON/bridge round trips,
  malformed-lane recovery, indexed normal-lane insertion, guarded child targets, source/child
  reorder and delete behavior, independent child visual/grade/height state, undo restoration, and
  atomic Qt-compatible track crossfades with subframe overlap, mask-child timing, overflow guards,
  generated-lane rejection, and undo/redo
- `test_imgui_project_history`: 30 passed after lifecycle and history-navigation additions,
  including New/Save As/Rename, symlink-escape rejection, atomic activation, stale-session
  rejection after rename, marker-commit cleanup, post-commit load rollback, rename recovery,
  legacy/delta entry navigation, redo preservation, stale/escaped rejection, paired rollback,
  concurrency-validated legacy transcript overrides, and History-tab wiring
- `test_imgui_standalone_export`: 7 passed, including explicit output-format muxer selection when
  the filename extension disagrees and Qt-free synthetic audio mixing coverage for timeline/source
  mapping, render-sync adjustment, gain/mute policy, and clip edge fades
- `test_imgui_standalone_render`: 11 passed, including neutral facade/source-boundary, standalone
  stream probing with still and image-sequence dimensions, image-sequence-directory probe/render,
  Scale-to-Fill shell wiring, visible canonical-owner marker hit-testing, reload-safe marker
  popup/modal contracts, stable-path audio-topology replacement, idle audio timeline status,
  derived-sidecar refresh coverage, Grade curve-point/lock/smoothing/normalization source
  contracts, and generated-child presentation, drop, reorder, drag-affordance, height, and
  grading-preview contracts
- `test_imgui_neutral_boundary`: executable and strict no-Qt linkage check passed; its compile-time
  contract covers the public audio facade, frame importer, preview, and editor-runtime types, and
  its runtime assertions cover base, interpolated, held, and endpoint full-grading evaluation plus
  curve sanitization, non-finite Qt bounds, deterministic LUT/normalization behavior, and shared
  three-point tone/curve mapping
- `test_grading_keyframes`: 15 passed, including direct Qt-versus-neutral evaluator parity for
  endpoints, interpolation, hold behavior, all nine tonal fields, curve topology fallback,
  curve flags, independently keyed opacity, fixed golden linear/smoothed LUTs, Qt wrapper parity,
  bounded normalization, and non-finite input handling
- `test_media_drag_drop`: 21 passed, retaining Qt generated-mask-track reconciliation, hierarchy
  reorder, aggregate ownership, clipboard, split, deletion, and presentation coverage
- `test_imgui_vulkan_import_boundary`: executable and strict no-Qt linkage check passed for the
  real external-frame importer facade and its shared Vulkan core
- `test_frame_payload_core`: standalone frame ownership/lifetime assertions and the strict no-Qt
  linkage check passed
- `test_frame_handle`: 11 passed, including the source-compatible QRhi adapter API, null CPU
  payload behavior, shared ownership, crop preservation, and memory accounting
- `test_transcript_document_core`: standalone projection assertions and the strict no-Qt linkage
  check passed
- `test_transcript_cut_session_core`: added for source/status/audio-stream resolution, editable
  creation, v0/v1/v2/v10 numeric catalog ordering, custom labels, persisted custom/stale-cut handling,
  outside-cut rows, file-stamp invalidation, neutral command/JSON round trips, rich-render wiring,
  and strict no-Qt linkage; its assertions and strict no-Qt linkage check passed
- `test_transcript_tab_follow`: 16 passed, including shared-core read-only row projection and stable
  Qt word identity after deletion in addition to follow, gap, outside-cut, overlay-refresh, version
  deletion, and speech-filter behavior
- `test_audio_mix_policy`: 15 passed, including Qt export/preview clip-fade equivalence, atomic timeline replacement, stale mixer and
  time-stretch rejection, generation-scoped terminal warmup failure, source-cache invalidation,
  and ring-buffer reset coverage
- `test_audio_time_stretch_cache`: 9 passed, including canceled stale writes and guarded commit
  refusal without damage to an existing sidecar
- `test_preview_geometry`: 16 passed after separating QWidget-only preview transform helpers
- `test_imgui_binary_no_qt_widgets`: passed for the current `jcut_imgui`
- `test_imgui_binary_no_qt_network`: passed for the current `jcut_imgui`
- `test_imgui_binary_no_qt_concurrent`: passed for the current `jcut_imgui`
- `test_media_color_passthrough`: 11 passed, including ordered/clamped parallel image-sequence
  batch loading
- `test_media_root_contract`: 8 passed; preserves the ImGui `mediaItems` extension across Qt state
  application and serialization in addition to the shared workspace-root contracts
- `test_imgui_cmake_qt_boundary`: passed; the generated ImGui runtime target interfaces, including
  `jcut_editor_core`, expose no Qt Widgets, Qt Network, Qt Concurrent, Qt GuiPrivate, or
  `jcut_frame_handle_qrhi_adapter` dependency

The generated MOC units also show the intended separation:

- `jcut_preview_decode_runtime`: `AsyncDecoder`, `MemoryBudget`, and `TimelineCache`
- `editor_test_support`: `ProjectManager` and `TranscriptTab`

The focused boundary checks now pass:

```bash
python3 tests/check_imgui_binary_no_qt.py --component Widgets build/jcut_imgui
python3 tests/check_imgui_binary_no_qt.py --component Network build/jcut_imgui
python3 tests/check_imgui_binary_no_qt.py --component Concurrent build/jcut_imgui
python3 tests/check_imgui_cmake_qt_boundary.py build/tests/imgui_runtime_link_interfaces.txt
python3 tests/check_imgui_binary_no_qt.py build/tests/test_imgui_neutral_boundary
python3 tests/check_imgui_binary_no_qt.py build/tests/test_imgui_vulkan_import_boundary
python3 tests/check_imgui_binary_no_qt.py build/tests/test_frame_payload_core
python3 tests/check_imgui_binary_no_qt.py build/tests/test_transcript_document_core
python3 tests/check_imgui_binary_no_qt.py build/tests/test_transcript_cut_session_core
```

The strict final-binary check is intentionally listed as unresolved, not as a passing result:

```bash
python3 tests/check_imgui_binary_no_qt.py build/jcut_imgui
```

It currently exits with failure because `build/jcut_imgui` still links Qt6 Gui and Core through
preview/audio and legacy runtime support; Qt6 DBus is also present transitively.

Neither the neutral public headers nor removal of Qt Widgets establishes functional parity. Rich
preview and preview audio still reuse Qt-backed implementations, and the incomplete workflows in
the parity matrix remain exit blockers.

## Parity Exit Criteria

The ImGui implementation is on par only when all of the following are true:

- `jcut_imgui` passes the strict no-Qt binary check.
- Every supported Qt editor surface has a connected ImGui equivalent.
- No visible ImGui control is presented as functional while discarding user input.
- Equivalent Qt and ImGui command sequences produce equivalent project snapshots.
- Complex projects roundtrip between shells without state or history loss.
- Preview and export results are equivalent for the same project and settings.
- Timeline editing, shortcuts, drag/drop, context actions, and undo/redo are functionally
  equivalent.
- Layout, preferences, dirty-state handling, and project lifecycle behavior are covered by smoke
  tests.
- Shared parity and shell-level test suites pass.
