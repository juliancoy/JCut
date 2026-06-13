# Jason Removal Plan

## Goal

Remove excessive reliance on JSON structures from preview, playback, speaker framing, and FaceDetections critical paths.

JSON is allowed at serialization boundaries: project files, transcript files, REST payloads, debug snapshots, migration input/output, and on-disk artifacts where JSON/CBOR remains the chosen interchange format. JSON must not be the in-memory representation used by frame-by-frame preview, playback, overlay lookup, speaker framing sampling, or candidate lookup.

Law: live playback must never decode or materialize JSON. Decode JSON/CBOR exactly once at a load or artifact boundary into compact typed memory: structs, vectors, indices, and caches. Playback and preview hot paths may only do bounded lookups over those typed arrays.

The final runtime shape should be:

- Disk: compact artifact manifests plus indexed artifact data.
- Load boundary: decode JSON/CBOR once into typed models.
- Memory: typed structs, vectors, indices, and caches.
- Critical path: lookup typed models by clip, source frame, track id, and speaker id without constructing `QJsonObject`, `QJsonArray`, or `QJsonDocument`.
- Debug/API: project typed state into JSON only when a human or REST client asks for it.

## Current Violations

### Preview FaceDetections Overlay

`vulkan_preview_surface_facedetections.cpp` still performs JSON-backed work from the live overlay refresh path.

Problems:

- `loadFacestreamTracksForClip()` includes `sourceFrame` in its cache signature, so it can invalidate and reload every frame.
- The preview path still calls `TranscriptEngine::loadFacestreamProcessedArtifact()` and `TranscriptEngine::loadFacestreamArtifact()` while refreshing overlays.
- Raw detection preview still calls `rawFramesNearFrameForContinuityRoot()` and iterates `QJsonArray` / `QJsonObject`.
- Debug data is built as `QJsonObject` while servicing the hot lookup.

Required final shape:

- Introduce a `FacedetectionsClipRuntimeCache` keyed by transcript path, artifact revision, clip id, overlay source, and relevant debug flags, not by source frame.
- Cache typed `FacestreamTrack` models and typed frame-detection indices separately.
- Per-frame overlay refresh should only do numeric frame lookup and interpolation.
- Build `m_lastFacedetectionsQueryDebug` only from already-computed counters, and only for selected/debug paths.

### Raw Detection Frame Access

`facedetections_continuity_artifacts.cpp` reads indexed frame records but returns `QJsonArray`.

Problems:

- `rawFrameRecordsNearPath()` materializes `QJsonObject` records from indexed artifacts.
- `rawFramesNearFrameForContinuityRoot()` exposes JSON as the runtime API.
- Raw detection preview then reparses each detection row from JSON.

Required final shape:

- Add typed models:
  - `FacestreamDetection`
  - `FacestreamFrameDetections`
  - `FacestreamFrameDetectionIndex`
- Add typed APIs:
  - `frameDetectionsNearFrameForRoot(...)`
  - `frameDetectionsAtOrNearFrameForRoot(...)`
  - `frameDetectionSummariesForRoot(...)`
- Keep JSON/CBOR decoding inside the artifact reader only.
- Return typed frame/detection vectors from runtime APIs.
- Keep JSON projection helpers only for tests, debug panels, and migration reports.

### Continuity Stream Construction

`continuityStreamsNearFrame()` and `continuityStreamsForAssignments()` still construct JSON streams through `buildContinuityStreams()`.

Problems:

- The Vulkan preview track path has been moved to typed `continuityTrackModelsNearFrameForRoot()`, but other callers still depend on JSON streams.
- Assignment lookup and playhead candidate UI can still call the JSON stream construction path.
- `buildContinuityStreams()` remains an attractive nuisance for new runtime code.

Required final shape:

- Replace JSON stream APIs with typed APIs:
  - `continuityTrackModelsForAssignments(...)`
  - `continuityTrackSummaryModelsForRoot(...)`
  - `continuityTrackModelsNearFrameForRoot(...)`
- Keep `buildContinuityStreams()` only as a boundary/debug adapter, or remove it if no longer needed.
- Mark JSON stream APIs as test/debug only until removed.
- Update all runtime call sites to use typed models.

### Speaker Framing Sampling

`editor_shared_keyframes.cpp` samples `QJsonObject` streams in `streamSampleAtFrame()`.

Problems:

- The assigned continuity cache stores `QVector<QJsonObject>`.
- Each sample converts JSON keyframes into temporary typed points.
- This can run under playback/render timing where allocations and JSON conversion are unacceptable.

Required final shape:

- Replace `streamSampleAtFrame(const QJsonObject&)` with typed sampling:
  - `streamSampleAtFrame(const FacestreamTrack&)`
  - or `sampleFacestreamTrackAtFrame(...)`.
- Replace `AssignedContinuityStreamsCacheEntry::streams` with typed `QVector<FacestreamTrack>`.
- Build typed assigned-track caches once per artifact revision.
- Store precomputed sorted keyframe vectors and typical frame step in the model.
- Make the sample function allocation-free for the steady-state case.

### Speakers Tab Playhead Candidate Refresh

`tracks.cpp` still uses JSON continuity streams in playhead candidate and panel paths.

Problems:

- `refreshPlayheadTrackCandidatesList()` computes `playbackActive` but still performs JSON lookup work.
- `continuityStreamsForClip()` caches `QJsonArray`.
- Raw detections panel expects inline `raw_frames` JSON and does not treat indexed typed artifacts as the primary source.

Required final shape:

- Do not refresh expensive candidate lists during active playback unless explicitly requested.
- Replace `m_continuityStreamsCache` with typed track-summary and track-model caches.
- Use typed frame detection access for raw detections panel.
- Convert typed data to JSON only when displaying "full JSON" debug text.

## Implementation Phases

### Phase 1: Define Runtime Models

Create one canonical in-memory model layer for FaceDetections and speaker framing.

Tasks:

- Extend `facedetections_types.h` with complete runtime models for tracks, keyframes, frame detections, detection summaries, identity assignments, and per-clip artifact metadata.
- Add explicit frame domain fields to typed structs.
- Add artifact revision metadata to runtime caches.
- Add conversion helpers that decode JSON/CBOR artifact records into typed models once.
- Add debug projection helpers named clearly, for example `facestreamTrackToDebugJson(...)`, so JSON construction is visibly non-runtime.

Acceptance criteria:

- No critical-path code needs to inspect `"frame"`, `"keyframes"`, `"detections"`, `"track_id"`, or `"stream_id"` strings directly.
- JSON field access for FaceDetections is contained in artifact readers, migration tools, tests, and debug projection helpers.

### Phase 2: Replace Preview Overlay Path

Move the Vulkan preview FaceDetections path to a persistent typed cache.

Tasks:

- Replace frame-keyed overlay cache invalidation with artifact-revision keyed cache invalidation.
- Load processed/raw artifact manifests once per revision.
- Populate typed track and raw detection indices once.
- Make `refreshFacestreamOverlays()` perform only typed lookup, filtering, interpolation, and box construction.
- Ensure raw detections use typed `FacestreamFrameDetections`.

Acceptance criteria:

- Perf capture during playback does not show `buildContinuityStreams()`, `QJsonObject`, `QJsonArray`, or `QJsonDocument` under `VulkanPreviewSurface::refreshFacestreamOverlays()`.
- Overlay visibility changes do not reload artifacts unless the artifact revision changed.
- Raw detection display remains correct for indexed `.idx` / `.dat` artifacts.

### Phase 3: Replace Speaker Framing Path

Remove JSON from speaker framing and transform sampling.

Tasks:

- Replace `QVector<QJsonObject>` assigned continuity cache with typed `QVector<FacestreamTrack>`.
- Replace `streamSampleAtFrame(const QJsonObject&)` with typed track sampling.
- Precompute sorted frames and typical frame step at cache-build time.
- Keep smoothing and gap-bridging logic unchanged.

Acceptance criteria:

- Speaker framing playback does not allocate JSON objects per frame.
- Existing speaker framing tests pass.
- Add a test that samples assigned continuity tracks from indexed artifacts without constructing JSON streams.

### Phase 4: Replace Speakers Tab Runtime Lookups

Move UI panels and playhead candidate lists to typed access.

Tasks:

- Replace `continuityStreamsForClip()` with typed `continuityTrackModelsForClip()` or summaries, depending on UI need.
- Replace `refreshPlayheadTrackCandidatesList()` with typed near-frame summaries.
- Skip or throttle expensive UI candidate refresh while playback is active.
- Replace raw detections panel scan of inline `raw_frames` with typed frame lookup.
- Keep "inspect full JSON" as a debug projection generated on selection, not during refresh.

Acceptance criteria:

- Playback active state prevents unnecessary UI JSON work.
- Candidate list and raw detections panel work against indexed artifacts.
- UI refresh counters remain bounded during playback.

### Phase 5: Restrict Legacy JSON APIs

Make the old JSON APIs hard to misuse.

Tasks:

- Move JSON stream APIs behind names that clearly indicate debug/boundary use.
- Remove runtime includes of `QJsonArray` / `QJsonObject` where only typed models should be needed.
- Add grep-based CI checks or tests for forbidden critical-path JSON usage.
- Delete unused JSON adapters after all runtime call sites move.

Acceptance criteria:

- `vulkan_preview_surface_facedetections.cpp`, `editor_shared_keyframes.cpp`, and runtime portions of `tracks.cpp` do not use JSON containers for FaceDetections lookup.
- `continuityStreamsNearFrame()` and `continuityStreamsForAssignments()` are deleted or restricted to test/debug code.
- `buildContinuityStreams()` is not reachable from playback or preview.

## Testing Requirements

Automated tests must cover:

- Indexed track artifact loading into typed models.
- Indexed raw detection artifact loading into typed frame models.
- Near-frame lookup over typed tracks.
- Raw detection exact/nearest/held-frame lookup.
- Speaker assignment lookup into typed tracks.
- Speaker framing sampling from typed tracks.
- Cache invalidation when `.idx`, `.dat`, transcript, identity, or processed sidecar revisions change.
- No fallback to legacy inline JSON in runtime paths.

Add a regression test that fails if a preview/playback call path invokes JSON stream construction. If direct call interception is impractical, add a narrow unit test around the runtime API surface and a source-level check for forbidden calls in known hot files.

## Profiling Requirements

After implementation, run call stack capture during active playback with FaceDetections overlays enabled.

Expected profile:

- `VulkanPreviewSurface::refreshFacestreamOverlays()` should show typed lookup and interpolation.
- `sampleFacestreamTrackAtFrame()` should be allocation-light and not show JSON conversion.
- No `buildContinuityStreams()` under preview/playback.
- No dominant `QJsonObject`, `QJsonArray`, `QJsonDocument`, CBOR decode, or artifact load work under per-frame preview.

## Boundary Rules

Allowed JSON:

- Project save/load.
- Transcript save/load.
- REST request/response payloads.
- Debug snapshots and profile responses.
- Migration tools.
- Tests and fixtures.
- On-disk artifact serialization, if decoded once at the boundary.

Forbidden JSON:

- Per-frame preview overlay lookup.
- Per-frame speaker framing lookup.
- Playback candidate lookup.
- Runtime FaceDetections track/detection matching.
- In-memory caches for tracks, detections, assignments, or keyframes.

## Completion Definition

This migration is complete when JSON is no longer the internal data model for FaceDetections or speaker framing runtime behavior.

The repository should make this mechanically obvious: runtime code consumes typed models, artifact readers own serialization, debug code owns JSON projection, and perf captures do not show JSON construction in the critical path.
