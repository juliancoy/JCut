# Startup Sequence

This document describes how the current JCut startup path loads project state,
clips, tracks, timeline state, preview state, and audio state. It reflects the
current code path in `EditorWindow`, `setupStartupLoad`,
`loadStartupStatePayload`, and `applyStateJson`.

## High-Level Flow

1. `EditorWindow` construction builds the UI shell synchronously.
2. The control server and audio engine are initialized before project state is
   loaded.
3. Startup project/state loading is posted with `QTimer::singleShot(0)` so the
   window can finish construction before JSON parsing and timeline binding.
4. The active project state is loaded on a worker thread.
5. `applyStateJson` parses settings, clips, tracks, render-sync markers, ranges,
   and playback/debug preferences.
6. Timeline clips and tracks are bound to `TimelineWidget` with callbacks
   temporarily disabled to avoid expensive intermediate refreshes.
7. During startup, preview/audio binding and the initial seek are deliberately
   deferred to later event-loop turns.
8. Deferred startup work binds timeline media into preview/audio, applies panel
   state, reconciles missing media paths, schedules transcript companion
   backfill, and starts lower-priority UI/profile warmups.

## Synchronous Construction

`EditorWindow::EditorWindow` starts the startup profile timer and records
milestones through `startupProfileMark`. The constructor builds and wires the
main application surfaces in this order:

- Window chrome.
- Main layout, including explorer pane, editor pane, preview, timeline, tabs,
  and inspector pane.
- Inspector widget bindings.
- Playback timers.
- Shortcuts, heartbeat, state-save timer, and deferred seek timers.
- Control server.
- Audio engine.
- Speech filter, track inspector, preview controls, tabs, and inspector refresh
  routing.
- Startup load scheduling.

The constructor marks `ctor.sync_complete` after synchronous setup completes.
At that point the UI shell exists, but project clips, tracks, preview media, and
audio media may still be pending.

## Control Server Startup

`setupControlServer` constructs `ControlServer` before startup project loading.
Its callbacks expose fast state, full state, history, profiling, pipeline data,
playhead control, and playback/debug configuration.

The current diagnostics relevant to startup are:

- `/health`: fast process/editor status; in the current build this includes
  `startup_readiness`.
- `/startup/readiness`: cheap startup readiness snapshot from the fast callback.
- `/profile`: cached or live profile data. Live profiling is intentionally
  deferred during playback unless forced/blocking.
- `/profile/startup`: startup profile event/ranking data after a cached profile
  exists.
- `/diag/perf`: performance diagnostics with fast snapshot data and startup
  readiness.

These endpoints report milestones and readiness state; they do not make all
startup work synchronous.

## Project State Source Order

`setupStartupLoad` posts startup loading to the event loop, then:

- Loads project metadata from the configured project folders.
- Resolves the current project id, state file path, and history file path.
- Starts `loadStartupStatePayload` on a worker thread.

`loadStartupStatePayload` prefers the direct state file over history:

- If the state file parses to a non-empty root object, that root is used and
  history loading is deferred.
- If the state file is empty/unavailable, history is loaded.
- History entries are sanitized before use so heavy per-clip
  `speakerFramingKeyframes` payloads are removed from startup history data.
- The current history entry becomes the startup root when direct state is not
  available.

This means normal startup optimizes for the current state snapshot first, then
loads heavier history data later.

## Clip Loading

`applyStateJson` parses timeline clips from the root JSON field named
`timeline`.

For each object:

- `clipFromJson` constructs a `TimelineClip`.
- Missing negative `trackIndex` values are replaced with the current loaded clip
  index.
- Clips are kept when they have a media file path or are title clips.

During startup, missing-media reconciliation is not performed before initial
timeline binding. It is deferred to a later event-loop turn. On non-startup
state application, missing media reconciliation may run inline before binding.

The startup milestone pair is:

- `apply_state.timeline_parse.begin`
- `apply_state.timeline_parse.end`, with `loaded_clip_count`

## Track Loading

`applyStateJson` parses timeline tracks from the root JSON field named `tracks`.

For each track object:

- `name` defaults to `Track N`.
- `height` is clamped to at least 28 pixels.
- `visualMode` is parsed when present.
- Legacy `visualEnabled=false` maps to hidden visual mode when `visualMode` is
  absent.
- `audioEnabled` defaults to true.

The parsed tracks are normal timeline tracks. Face/continuity sidecar data is
not fully loaded as part of this JSON track parse. Sidecar-backed face and
continuity data is loaded lazily or in bounded windows by the preview/speakers
paths.

The startup milestone pair is:

- `apply_state.tracks_parse.begin`
- `apply_state.tracks_parse.end`, with `loaded_track_count`

## Timeline Binding

Timeline binding happens in `applyStateJson` after settings, clips, tracks,
render sync markers, export ranges, and preview/audio preferences have been
parsed.

The current code temporarily disables expensive `TimelineWidget` callbacks while
bulk state is applied:

- `clipsChanged`
- `selectionChanged`
- `trackLayoutChanged`
- `renderSyncMarkersChanged`
- `exportRangeChanged`

Then it applies:

- Tracks.
- Clips.
- Timeline zoom.
- Vertical scroll.
- Export ranges.
- Render sync markers.
- Selected clip id.
- Slider range synchronization.

Callbacks are restored afterward. `trackLayoutChanged` is invoked once manually
after the bulk bind so dependent UI can refresh from the final state instead of
from partial intermediate state.

The startup milestone pair is:

- `apply_state.timeline_bind.begin`
- `apply_state.timeline_bind.end`, with timeline clip and track counts

## Deferred Preview And Audio Binding

The startup path intentionally does not bind preview/audio media inline inside
`applyStateJson`.

During startup:

- `apply_state.preview_bind.deferred` is marked.
- `apply_state.audio_bind.deferred` is marked when an audio engine exists.
- `apply_state.seek.deferred` is marked.

After `applyStateJson` returns, a posted event-loop callback computes effective
playback ranges and calls:

```cpp
bindTimelineMediaState(selectedClipId, deferredPlaybackRanges, currentFrame, true);
```

`bindTimelineMediaState` then:

- Sends clip count, tracks, clips, export ranges, render sync markers, and
  selected clip id to the preview surface in a preview bulk update.
- Sends timeline clips, export ranges, transcript normalize ranges, render sync
  markers, speech filter settings, playback warp mode/rate, transcript
  normalize state, and audio dynamics settings to the audio engine.
- Seeks audio when requested.
- Sets the current frame when requested.

This split is intentional: the initial state load binds the timeline quickly,
then media-heavy preview/audio work happens after the UI has had a chance to
process the startup event queue.

## Preview And Video Readiness

The preview surface receives timeline media state from
`bindTimelineMediaState`. Playback applies samples through
`EditorWindow::setCurrentPlaybackSample`, which updates:

- Absolute and filtered playback samples.
- Fast current frame.
- Timeline current frame.
- Preview current playback sample.
- Seek slider and timecode label.
- Inspector/playhead sync, throttled during playback.

The startup readiness tracker marks `video.playback_sample_applied` the first
time a playback sample is applied to the preview after process startup.

This does not mean every future frame is decoded. It means the editor has
applied at least one playback sample to the preview path.

## Audio Readiness

The audio engine is constructed during synchronous setup. Timeline media is sent
to the audio engine later through `bindTimelineMediaState`.

On playback start, `setPlaybackActive(true)`:

- Warms preview lookahead before entering playback.
- Pushes current export ranges and transcript normalize ranges to audio.
- Applies speech filter settings, playback warp mode/rate, transcript normalize
  state, and audio dynamics settings.
- Starts audio if playable audio exists and the requested speed/warp mode can be
  handled.
- Marks `audio.start.invoked` once on the first audio start.
- Advances one frame, marks `playback.first_tick`, starts the playback timer,
  and sets preview playback state.

Current time-stretch behavior uses precomputed sidecars when time-stretch mode
is active. Sidecars are read before declaring a time-stretch cache miss, and
time-stretch computation is not supposed to run on the real-time mix/playback
path.

## Face, Speaker, And Continuity Data

Timeline track JSON loading only restores timeline rows and their visual/audio
state. Face tracks, speaker tables, and continuity overlays are handled by
separate paths:

- Speakers/FaceDetections UI uses summary-first loading for sidecar artifacts.
- Full per-track JSON is loaded lazily when a summary-backed row is selected.
- Preview facestream overlays load continuity data around the current frame.
- Raw face detections are read in bounded windows near the current source frame
  when record-style artifacts are available.

This is why a timeline can be bound while face/continuity overlay details are
still loading or waiting for a preview refresh.

## Deferred Work After State Load

After startup state is applied, additional work is scheduled rather than forced
inline:

- Deferred history load when direct state was used.
- Transcript `.txt` companion backfill.
- Deferred startup panel state.
- Missing-media reconciliation from the resolved media root.
- Preview/audio media binding and initial seek.
- Deferred UI warmup.
- Optimized profile/cache warmup.
- Autosave startup.

This keeps the constructor and first state bind from doing all heavyweight work
at once, but it also means startup should be interpreted as a staged process.

## Startup Readiness Tracking

`startupProfileMark` records ordered startup events with elapsed and delta
times. It also feeds `startupReadinessMark`, which maintains a compact
readiness object.

Current readiness fields include:

- `ui_constructed`: `ctor.sync_complete` was reached.
- `project_state_loaded`: project state reached loaded/autosave/complete.
- `timeline_bound`: timeline clips/tracks were bulk-bound.
- `audio_bind_scheduled`: audio binding was completed or deferred.
- `preview_bind_scheduled`: preview binding was completed or deferred.
- `startup_load_complete`: startup load completion was marked.
- `first_playback_tick`: playback advanced at least once.
- `audio_started`: audio start was invoked at least once.
- `video_playback_sample_applied`: preview received at least one playback
  sample.
- `ready_to_play`: currently true when `startup_load_complete` and
  `video_playback_sample_applied` are both true.

The readiness object is a coarse operational signal, not a guarantee that every
clip has decoded, every sidecar has hydrated, or every overlay has been drawn.

## Important Caveats

- Startup is intentionally staged. A completed state load is not the same thing
  as fully decoded media.
- Timeline tracks are restored from project JSON first; sidecar-backed
  face/continuity details are loaded separately.
- Preview/audio binding is deferred during startup to reduce synchronous UI
  stalls.
- The current readiness trace is milestone-based. It does not automatically time
  every function or every decoder/sidecar operation.
- REST diagnostics reflect the running process. If the executable was rebuilt
  while an older process is still running, the old process will not expose new
  startup fields or behavior until it is restarted.
