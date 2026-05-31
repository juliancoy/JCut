# FaceDetections Storage Migration

This document tracks the migration away from large JSON payloads as the primary in-memory and on-disk representation for FaceDetections data.

## Problem

FaceDetections data is stored as compact sidecars that reference indexed raw artifacts. The final raw-artifact shape is explicit index/data pairs:

- `tracks.idx` / `tracks.dat`
- `detections.idx` / `detections.dat`

Readers should use the `.idx` files as the public artifact path. The `.dat` files contain compressed CBOR records addressed only through index offsets. Profiling showed the old full-payload path created heavy startup and playback CPU cost:

- compressed record inflation via `qUncompress` / `inflate`
- JSON parsing
- repeated conversion between JSON libraries and Qt JSON types
- allocator churn from generic object trees
- UI code holding or rebuilding large JSON payloads when it usually needs summaries or frame-local data

The practical goal is to make JSON an interchange/debug format only. Runtime code should use typed, indexed data structures.

## Target Ownership Model

### 1) `TranscriptEngine`

`TranscriptEngine` should remain the path and revision authority.

Responsibilities:

- resolve transcript sidecar paths
- expose artifact revision tokens
- load/save transcript documents

Non-responsibilities:

- owning parsed FaceDetections records
- caching large detection/track payloads
- exposing raw JSON as the main runtime API

### 2) FaceDetections Artifact Store

The `jcut::facedetections` layer should own typed artifact memory and artifact indexes.

This layer should provide APIs around typed concepts:

```cpp
struct FacestreamArtifactRef {
    QString transcriptPath;
    QString clipId;
    QString rawTracksPath;
    QString rawFramesPath;
    QString processedPath;
    qint64 revisionMs = -1;
};

struct FacestreamTrackSummary {
    int trackId = -1;
    QString streamId;
    QString source;
    qint64 minFrame = -1;
    qint64 maxFrame = -1;
    int keyframeCount = 0;
    qint64 typicalFrameStep = 1;
};

struct FacestreamKeyframe {
    qint64 frame = -1;
    float x = 0.5f;
    float y = 0.5f;
    float box = 0.2f;
    float confidence = 0.0f;
};

struct FacestreamTrack {
    FacestreamTrackSummary summary;
    QVector<FacestreamKeyframe> keyframes;
};
```

Expected query shape:

```cpp
QVector<FacestreamTrackSummary> trackSummaries(ref);
QVector<FacestreamTrack> tracksForAssignments(ref, trackIds, streamIds);
QVector<FacestreamFrameDetections> framesNear(ref, frame, window);
QVector<ContinuityStream> streamsNear(ref, frame, window);
```

### 3) UI View Models

`SpeakersTab` and preview code should consume typed summaries, tracks, and frame-local overlays.

Responsibilities:

- table rows
- selected ids
- small view models
- interaction state

Non-responsibilities:

- parsing sidecar artifacts
- owning raw artifact payloads
- passing full JSON records between UI methods

### 4) Serialization Adapters

JSON conversion should live at the edges:

- explicit migration tools for old artifacts
- debug/detail panels
- human-readable export/inspection tools

## Memory Tiers

### Manifest Cache

Small and cheap. Stores per transcript/clip:

- artifact paths
- schema/version
- clip id
- revision tokens
- counts

### Index Cache

Medium-sized and revision-keyed. Stores per artifact path:

- track summaries
- frame ranges
- record offsets
- compressed record sizes

This should be enough for table summaries, assignment matching, and range checks without loading full records.

### Hot Record Cache

Bounded LRU. Stores full typed records only after a caller asks for them:

- matching assignment tracks
- frame records near the playhead
- selected debug/detail payloads

Existing `loadedTrackRecordsByOffset` is the right idea, but it should store typed records instead of `QJsonObject`.

### UI Cache

Short-lived and view-specific. Stores rows, selection, pixmaps, and preview state only.

## Target On-Disk Format

Keep a small manifest readable:

```text
facedetections_artifact/
  manifest.json
  tracks.idx
  tracks.dat
  frames.idx
  frames.dat
  processed_streams.bin
```

The large data should be binary/indexed, not JSON.

Recommended split:

- `manifest.json`: schema, generator, source media, counts, frame domains, paths
- `tracks.idx`: fixed-size track summary records and offsets into `tracks.dat`
- `tracks.dat`: compact binary/CBOR track payloads
- `frames.idx`: frame number to offset/count records
- `frames.dat`: compact binary/CBOR frame detections
- `processed_streams.bin`: derived stream/keyframe data used by speaker UI and preview

Runtime readers should accept only current indexed artifacts. The migration executable is the only code path that should understand old monolithic or sequential record artifacts.

Migration command:

```bash
scripts/migrate_facedetections_artifacts.sh path/to/transcript.json
```

Preview only:

```bash
scripts/migrate_facedetections_artifacts.sh --dry-run path/to/transcript.json
```

## Migration Stages

### Stage 1: Remove Double JSON Materialization

Status: in progress.

Done:

- `json_io_utils::parseObjectBytes` now uses `QJsonDocument::fromJson` directly instead of parsing into `nlohmann::json` and recursively rebuilding Qt JSON.
- Assignment track lookup filters through the index and only loads matching full records.
- Shared typed FaceDetections structs now exist in `facedetections_types.h`.
- `tracks.cpp` now uses the shared `FacestreamTrack` / `FacestreamKeyframe` structs instead of private duplicate cache structs.
- `continuityTrackSummaryModelsForRoot()` provides a typed summary API over the existing compatibility reader.
- Generated artifacts now use `tracks.idx`/`tracks.dat` and `detections.idx`/`detections.dat`.
- Runtime raw artifact readers require indexed artifacts and do not scan sequential record files.
- `jcut_migrate_facedetections_artifacts` converts old monolithic JSON, compressed JSON record, and CBOR record artifacts directly to indexed artifacts.
- `scripts/migrate_facedetections_artifacts.sh` wraps the migration executable for one or more transcript files.

Remaining:

- Move the new typed index structures into a dedicated artifact-store module.
- Replace remaining generic JSON call sites with typed wrappers where practical.
- Move UI table refresh and preview reads from JSON arrays to typed summary/track/frame APIs.

### Stage 2: Centralize Typed FaceDetections Memory

Create a dedicated artifact store module under `jcut::facedetections`.

Goals:

- one place owns artifact index caches
- one place owns hot record caches
- public APIs return typed data first
- JSON wrappers become compatibility shims

Candidate files:

- `facedetections_artifact_store.h`
- `facedetections_artifact_store.cpp`

### Stage 3: Move UI Code To Typed View Models

Move local cache structs such as `CachedFacestreamTrack` / `CachedFacestreamKeyframe` out of `tracks.cpp` and into shared typed headers.

Goals:

- `SpeakersTab` consumes typed stream/track data
- preview code consumes typed frame-local overlays
- table refreshes no longer depend on large `QJsonArray` payloads

### Stage 4: Add Frame Indexes

The profile still shows JSON parsing under frame-near-playhead reads.

Goals:

- index raw frame records by frame number
- seek directly to relevant frame records
- fully parse only the small frame window needed for the current UI/playhead

### Stage 5: Write New Binary Artifacts

Status: complete for generated FaceDetections raw artifacts.

New generation writes indexed `.idx` files plus compressed CBOR `.dat` payload files.

Goals:

- avoid JSON parsing for large track/frame payloads
- provide a migration tool for existing debug artifacts
- continue splitting the data into explicit `.idx` and `.dat` files rather than sequential records

### Stage 6: Retire Legacy JSON Runtime Paths

After compatibility has been proven:

- remove runtime dependence on full JSON track/frame payloads
- keep JSON only for debug export/import
- limit `QJsonObject` APIs to compatibility adapters

## Current Profiling Notes

Before the first migration changes, the profile was dominated by:

- `jsonio::fromJson`
- `QJsonObject::insert`
- `nlohmann::json` lexer/parser work
- `qUncompress` under `trackArtifactIndexForPath`

After the first changes:

- the `jsonio::fromJson` / `QJsonObject::insert` hot path dropped out
- assignment lookup stopped parsing every track record
- record payloads moved from compressed JSON to compressed CBOR
- remaining heat is mostly record inflation, narrow CBOR SAX lexing for track indexes, frame-near-playhead record parsing, and unrelated audio/file-sequence work

## Engineering Rule

New FaceDetections runtime APIs should not expose `QJsonObject` or `QJsonArray` as the primary data model. They may expose JSON only for compatibility, debug inspection, and serialization boundaries.
