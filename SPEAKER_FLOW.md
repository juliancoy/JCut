# Speaker Flow

## Purpose
This document describes the current speaker / FaceStream pipeline as it exists in the app today.

It is intentionally shorter than the older planning doc. The goal is to document:
- the user-facing flow
- the durable sidecars
- the debug-run artifacts written by identity assignment
- the source-of-truth contract used at runtime

It does not describe speculative UI or future tooling.

## Terms
- `Detection`: one face observation in one frame.
- `Continuity Track`: identity-agnostic grouping of detections over time.
- `Identity Cluster`: one or more continuity tracks believed to be the same person.
- `Speaker Assignment`: resolved mapping from continuity tracks to transcript speaker IDs.
- `Speaker Tracking`: runtime framing/stabilization behavior that consumes the resolved speaker assignment.

## Main User Flow
### 1. Prepare an editable transcript cut
1. Select a clip with usable video and audio.
2. Create or switch to an editable transcript cut.
3. Transcribe the clip so transcript segments/words and speaker IDs exist.

### 2. Clean up transcript speaker data
1. Open `Speakers`.
2. Optionally run the AI speaker-name / organization / cleanup actions.
3. Apply accepted profile updates.

### 3. Generate continuity tracks
1. Click `Generate FaceStream`.
2. The generator produces:
   - raw detections
   - processed continuity tracks
3. The transcript gets a FaceStream sidecar.
4. `Rebuild Continuity Tracks` is only a maintenance path from existing raw data.

### 4. Assign continuity tracks to transcript speakers
There are now two supported paths.

#### Path A: Batch identity assignment
1. Click `Assign Speaker Identity`.
2. The app extracts representative crops per continuity track.
3. The app embeds those crops with ArcFace NCNN.
4. Tracks are clustered by cosine similarity.
5. The assignment dialog lets the user map identity clusters to transcript speaker IDs.
6. Applying the dialog persists the resolved `track_id -> speaker_id` map.

#### Path B: Seeded matching from one known track
1. Select a speaker in `Speakers`.
2. Select a continuity-track row in the FaceStream table.
3. Right-click and choose `Find Matching Tracks for ...`.
4. The app uses the selected track as the seed, embeds all track crops with ArcFace NCNN, and scores every other track against the seed.
5. A review dialog shows:
   - the seed track
   - prechecked `auto_match` tracks
   - optional `review` tracks
6. Applying the dialog assigns the checked tracks to the selected speaker in one batch edit.

### 5. Use the resolved mapping at runtime
1. Enable `Speaker Tracking` for the desired speaker.
2. Optionally enable `Face Stabilize` for the clip.
3. Runtime framing and stabilization consume the resolved `track_id -> speaker_id` map, not raw detections and not raw identity clusters.

## Durable Sidecars
### FaceStream sidecar
File:
- `{transcript_basename}_facestream.bin`

Stores:
- raw detection payloads
- continuity-track payloads
- processed continuity stream data used by the Speakers UI

### Identity sidecar
File:
- `{transcript_basename}_identity.bin`

Stores:
- `identity_clusters_by_clip`
- `identity_assignments_by_clip`

This sidecar is the durable record for identity clustering and assignment review state.

## Transcript-Embedded Speaker Flow State
The active transcript document also stores speaker-flow state under:
- `speaker_flow.clips.<clip_id>`

Important subtrees:
- `machine_runs`
- `human_runs`
- `resolved_current`

`resolved_current.track_identity_map` is the runtime-facing resolved layer.

## Debug Run Layout
The batch `Assign Speaker Identity` flow writes debug runs under:

- preferred: `projects/<project_id>/debug/speaker_flow/<clip_id>/<run_id>/`
- fallback: `<derived root>/debug/speaker_flow/<clip_id>/<run_id>/`

`run_id` format:
- `YYYYMMDD-HHMMSS-<short_uuid>`

Each run has an `index.json` entrypoint plus stage artifacts.

## Current Debug Artifacts
### Stage 4: representative crop extraction
Files:
- `{videofilename}_facestream_track_candidates.json`
- `{videofilename}_facestream_track_crops/`

Contains:
- representative crops per continuity track
- crop metadata
- `track_id`
- crop frame / source frame / normalized box

### Stage 5: identity clustering
Product sidecar:
- `{transcript_basename}_identity.bin`

Contains:
- embedding model metadata
- `embedded_track_count`
- cluster rows
- pairwise diagnostics
- thresholds used for auto cluster vs review

Current defaults:
- `cosine >= 0.70`: `auto_cluster`
- `0.55 <= cosine < 0.70`: `review`
- `cosine < 0.55`: different

### Stage 6: assignment review
Files:
- `{videofilename}_assignment_table.json`
- `{videofilename}_assignment_decisions.json`

Product sidecar:
- `{transcript_basename}_identity.bin`

Contains:
- cluster review rows
- chosen speaker IDs
- overrides
- final `track_identity_map`

## Source Of Truth
Use this order:

1. `speaker_flow.clips.<clip_id>.resolved_current`
   - authoritative runtime `track_id -> speaker_id` mapping
2. `speaker_flow.clips.<clip_id>.human_runs`
   - user-driven assignment and audit history
3. `speaker_flow.clips.<clip_id>.machine_runs`
   - machine-produced evidence and suggestions
4. FaceStream sidecar raw/processed track data
   - motion grouping and detections only

Runtime speaker tracking should prefer the resolved map whenever it exists.

## Current Behavior Notes
- Continuity-track generation is identity-agnostic.
- Identity clustering is review assistance, not the runtime source of truth.
- The seeded `Find Matching Tracks` workflow is separate from batch cluster review, but writes into the same resolved assignment layer.
- Manual preview assignment is still supported by selecting a speaker and clicking a FaceStream box in preview.
- FaceStream generation can exist without identity assignment.
- Speaker-specific tracking quality depends on having a resolved assignment.

## What This Document Does Not Promise
These items were present in the older planning doc but are not guaranteed as current product behavior:
- a dedicated Speakers debug panel
- explicit export-bundle UI
- cluster split/merge editing UI
- retention-policy enforcement
- per-stage error JSON/TXT for every possible failure path

If those features are implemented later, document them separately once they are real.
