# Speaker Flow Debug Artefacts And Pipeline

## Goal
Provide a deterministic, stage-by-stage debug pipeline for the Speaker Flow so failures are diagnosable from one place without reading source code.

## Scope
Covers this end-to-end path:
1. Transcript ingestion and speaker IDs
2. AI name mining and speaker profile updates
3. Continuity FaceStream generation (`Generate FaceStream`, identity-agnostic)
4. FaceStream representative crop extraction (`Assign FaceStreams` / FaceFind)
5. Same-person track clustering from representative crops
6. Cluster-to-speaker assignment (auto + manual override)
7. Runtime tracking and Face Stabilize application

## Product Sidecars By Step
Product sidecar means a durable clip/transcript companion file used by normal app behavior. Debug-run artifacts are listed separately under `Stage Artefacts`.

| Step | Product sidecar |
| --- | --- |
| 1. Transcript ingestion and speaker IDs | none |
| 2. AI name mining and speaker profile updates | none |
| 3. Continuity FaceStream generation | `{transcript_basename}_facestream.bin` |
| 4. FaceStream representative crop extraction | none; debug-run crop artifacts only |
| 5. Same-person track clustering from representative crops | `{transcript_basename}_identity.bin` |
| 6. Cluster-to-speaker assignment | `{transcript_basename}_identity.bin` |
| 7. Runtime tracking and Face Stabilize application | none |

## Expected User Path
This is the intended end-to-end flow starting from a clip that has both video and audio.

### 1. Start With a Source Clip
Product sidecar: none.

1. Import/select a clip that includes both video and audio.
2. Confirm the clip is selected in the timeline.

### 2. Create/Select an Editable Transcript Cut
Product sidecar: none.

1. Open the `Transcript` tab.
2. Create a transcript cut version (or select an existing non-original editable cut).
3. Keep this editable cut active for all speaker and face operations.

### 3. Transcribe
Product sidecar: none.

1. Run transcription for the selected clip/cut.
2. Wait until transcript segments/words and speaker IDs are present.

### 4. Mine Transcript (AI)
Product sidecar: none.

1. Open the `Speakers` tab.
2. Click `Mine Transcript (AI)` to suggest better speaker names.
3. Optionally run `Find Organizations` and `Clean Assignments`.
4. Apply accepted suggestions so speaker profiles are populated.

### 5. Generate FaceStream (Tracking Data)
Product sidecar: `{transcript_basename}_facestream.bin`.

1. In `Speakers`, click `Generate FaceStream`.
2. The app detects and tracks face continuity, then writes one FaceStream per continuity track.
3. Confirm the Overview shows the FaceStream sidecar as present.

### 6. Match FaceStreams to Speaker IDs
Product sidecar: `{transcript_basename}_identity.bin`.

1. In `Speakers`, click `Assign FaceStreams`.
2. The app extracts one representative face crop per generated FaceStream track.
3. The app clusters tracks that appear to belong to the same person.
4. Review each identity cluster and its member FaceStream tracks.
5. Preferred: keep `Assign To (Auto)` when correct.
6. Backup: enter `Manual Speaker ID (Override)` when auto is wrong or missing.
7. Apply assignments.
8. Result: resolved FaceStream tracks map to transcript speaker IDs for speaker-aware tracking.

### 7. Refine FaceStream Generation (If Needed)
Product sidecar: same as Step 5 when regenerated; none if only reviewing settings.

1. In preflight, choose algorithm based on goal:
2. `DNN Auto (CUDA/CPU)` for strong general face detection.
3. `OpenCV Contrib CSRT` for stability on moderate motion (quality-first).
4. `OpenCV Contrib KCF` for lighter/faster tracking (speed-first).
5. Regenerate FaceStream after changing detector settings, dialogue-only range, stride, or crop/preview options.

### 8. Enable Speaker Tracking
Product sidecar: none.

1. Turn on `Tracking` for the speaker (Tracking chip/button).
2. Confirm the state shows FaceStream ready/active.

### 9. Set FaceBox Target
Product sidecar: none.

1. In `Speakers`, use `FaceBox (Yellow Box)` controls:
2. Set `Yellow Box X`, `Yellow Box Y`, and optional `Yellow Box Size`.
3. Toggle `Show FaceBox` to visually confirm target placement in Preview.

### 10. Enable Face Stabilize (Clip-Level)
Product sidecar: none.

1. Enable `Face Stabilize` for the selected clip.
2. Confirm status updates (`Face Stabilize: ON` and runtime/keys state).

### 11. Validate and Iterate
Product sidecar: none; repeating generation or assignment updates the Step 5 or Step 6 sidecars.

1. Scrub and play through speaker changes.
2. If tracking drifts:
3. Tune detector settings, regenerate FaceStream, re-run `Assign FaceStreams`, and re-check target placement.

### 12. Render
Product sidecar: none.

1. Once tracking and framing behavior are stable, proceed to render/export.

### User Path Notes
1. Preferred UX route: generate FaceStream once, then use `Assign FaceStreams` for track-to-speaker assignment.
2. Manual fallback is always available via `Manual Speaker ID (Override)`.
3. FaceStream generation is valid without identity assignment, but speaker-specific tracking requires a resolved `track_id -> speaker_id` map.
4. Speaker/face edits are intended for editable transcript cuts, not immutable original transcripts.

## Principles
1. Every stage writes a machine-readable artefact.
2. Every artefact has stable schema + version.
3. Every run has a single `run_id` that links all artefacts.
4. Failures are first-class: write error artefacts, not only logs.
5. UI exposes links to current run artefacts.
6. Explicit is better than implicit: every auto/default mapping must show its source.
7. Machine originals are immutable; human edits are layered as overrides.
8. A resolved layer is the source used by speaker-aware tracking and stabilization.

## Proposed Debug Artefact Root
`projects/<project_id>/debug/speaker_flow/<clip_id>/<run_id>/`

Where `run_id` format is:
`YYYYMMDD-HHMMSS-<short_uuid>`

## Artefact Naming Convention (Required)
All artefacts must use:
`{videofilename}_artefacttype.ext`

Examples:
1. `interview_take_03_transcript_snapshot.json`
2. `interview_take_03_facestream_tracks.json`
3. `interview_take_03_identity.bin`

Rules:
1. `videofilename` is the source clip basename without extension, sanitized to `[a-zA-Z0-9._-]`.
2. `artefacttype` is a stable snake_case token from this plan.
3. If multiple files of same type are required, append `_partN` or `_idxN` before extension.
4. Fixed-name files inside a generated FaceStream artifact directory, such as `facestream.part`, `tracks.bin`, and `summary.json`, are allowed when the parent artifact directory is registered in `index.json`.
5. Identity clustering and identity assignment state lives in a dedicated transcript sidecar: `{transcript_basename}_identity.bin`.

## Artefact Index (Required)
File: `index.json`

Purpose:
1. Entry point for all artefacts in a run.
2. Stage status summary and timings.
3. Version contract for parsers.

Minimum fields:
1. `schema_version`
2. `run_id`
3. `project_id`
4. `clip_id`
5. `transcript_path`
6. `started_at_utc`
7. `completed_at_utc`
8. `stage_status` (per-stage `ok|warn|error|skipped`)
9. `artefacts` (relative paths)
10. `errors` (high-level)

## Stage Artefacts

### Stage 1: Transcript Snapshot
Product sidecar: none. These files are debug-run artifacts only.

Files:
1. `{videofilename}_transcript_snapshot.json`
2. `{videofilename}_speaker_ids.json`

Contents:
1. Active transcript/cut metadata.
2. Speaker IDs discovered from segments/words.
3. Speaker word counts and timing spans.

### Stage 2: AI Name Mining
Product sidecar: none. These files are debug-run artifacts only.

Files:
1. `{videofilename}_name_mining_input.json`
2. `{videofilename}_name_mining_output.json`
3. `{videofilename}_name_mining_apply.json`

Contents:
1. Candidate names + confidence/rationale per speaker.
2. Which names were auto-applied.
3. Which existing-name overwrites were user-approved.
4. Which suggestions were rejected.

### Stage 3: Continuity FaceStream Generation
Product sidecar: `{transcript_basename}_facestream.bin`.

Files:
1. `{videofilename}_continuity_facestream_request.json`
2. `{videofilename}_continuity_facestream_output.json`
3. `{videofilename}_continuity_facestream_log.txt`
4. `facestream.part`
5. `tracks.bin`
6. `continuity_facestream.bin`
7. `summary.json`

Contents:
1. Identity-agnostic mode metadata (`mode=continuity_identity_agnostic`).
2. Scan range metadata and `only_dialogue` policy.
3. Detector params (`detector`, stride, frame range, threshold, candidate caps, ROI, area, and aspect filters).
4. Continuity tracks and per-track keyframe stream output.
5. Track IDs that are stable within run for deterministic traceability.
6. Native Vulkan runs may include live tuning metadata and runtime stats.
7. Preview/debug artifacts are sampled separately from detector processing via preview stride.

Artifact roles:
1. `{transcript_basename}_facestream.bin` is the durable product sidecar loaded by JCut runtime, preview overlay, speaker-aware tracking, and stabilization.
2. `continuity_facestream.bin` is the generator-to-editor import payload. After successful import into `{transcript_basename}_facestream.bin`, it is redundant for normal runtime.
3. `facestream.part` is a streaming checkpoint for resumable generation. It is useful while a run is active or interrupted, but redundant for normal runtime after successful import.
4. `tracks.bin` is raw track/frame diagnostic output used to explain or rebuild the continuity result. It is not required by normal runtime after successful import.
5. `summary.json` is profiling and run metadata. It is not required by normal runtime, but is useful for UI diagnostics and performance/debug review.

### Stage 4: FaceStream Representative Crop Extraction
Product sidecar: none. These files are debug-run artifacts only.

Files:
1. `{videofilename}_facestream_track_candidates.json`
2. `{videofilename}_facestream_track_crops/` (pngs)

Contents:
1. Source generated FaceStream artifact path.
2. One representative crop per generated FaceStream track.
3. Crop frame, source frame, normalized face box, score, `track_id`, and crop path.
4. No independent face scan here; this stage consumes generated FaceStream tracks only.

### Stage 5: Same-Person Track Clustering
Product sidecar: `{transcript_basename}_identity.bin`.

Files:
1. `{transcript_basename}_identity.bin`

Contents:
1. `schema = jcut_identity_v1`.
2. `identity_clusters_by_clip.<clip_id>`.
3. Visual embedding model and version used for each representative crop.
4. Per-track embedding metadata and crop provenance.
5. Cluster rows containing `cluster_id`, member `track_id` values, representative crop, confidence, and conflict flags.
6. Pairwise similarity or nearest-neighbor diagnostics sufficient to explain merge/split decisions.
7. Manual split/merge overrides if the user corrects clustering before speaker assignment.

How clustering happens:
1. For each FaceStream track, select representative crop(s) from high-confidence keyframes.
2. Run each crop through a face embedding model, such as ArcFace via NCNN.
3. Normalize each embedding vector.
4. Build a per-track embedding by averaging normalized crop embeddings when more than one crop is available.
5. Compare track embeddings using cosine similarity.
6. Merge tracks into the same `cluster_id` when visual similarity is above the same-person threshold.
7. Flag borderline similarity pairs for user review instead of silently merging.
8. Use transcript timing only as a weak secondary signal; visual embedding similarity is the primary clustering signal.

Recommended first-pass thresholds:
1. `cosine >= 0.70`: auto-cluster as likely same person.
2. `0.55 <= cosine < 0.70`: uncertain; keep separate or flag for manual review.
3. `cosine < 0.55`: treat as different people.

Example cluster artifact:
```json
{
  "clusters": [
    {
      "cluster_id": "person_001",
      "track_ids": [3, 7, 12],
      "confidence": 0.84,
      "representative_crop": "track_003.png",
      "status": "auto_clustered"
    },
    {
      "cluster_id": "person_002",
      "track_ids": [5],
      "confidence": 1.0,
      "status": "singleton"
    }
  ]
}
```

Professional defaults:
1. Prefer multiple crops per track when enough high-confidence keyframes exist.
2. Keep singleton clusters valid; a person may appear in only one FaceStream.
3. Never let clustering directly drive stabilization; stabilization consumes the final resolved track map.
4. Persist user split/merge edits separately from immutable machine clusters.

### Stage 6: Cluster Assignment (Identity Layer)
Product sidecar: `{transcript_basename}_identity.bin`. JSON assignment files are debug exports, not product sidecars.

Files:
1. `{transcript_basename}_identity.bin`
2. `{videofilename}_assignment_table.json` (debug export)
3. `{videofilename}_assignment_decisions.json` (debug export)

Contents:
1. `identity_assignments_by_clip.<clip_id>`.
2. Auto suggestion per identity cluster.
3. Manual override value if provided.
4. Validation outcome (accepted/rejected + reason).
5. Final resolved speaker identity ID per accepted cluster.
6. `cluster_id` and member `track_id` values persisted in rows for deterministic traceability.

Assignment output:
1. The user assigns each `cluster_id` to a transcript speaker ID.
2. Applying an assignment expands cluster membership into the final runtime map.
3. The final persisted map is `track_id -> speaker_id`, not `cluster_id -> speaker_id`.

Example resolved map:
```json
{
  "track_identity_map": [
    { "track_id": 3, "identity_id": "SPEAKER_00" },
    { "track_id": 7, "identity_id": "SPEAKER_00" },
    { "track_id": 12, "identity_id": "SPEAKER_00" },
    { "track_id": 5, "identity_id": "SPEAKER_01" }
  ]
}
```

### Stage 7: Runtime Sampling / Face Stabilize
Product sidecar: none. These files are debug-run artifacts only.

Files:
1. `{videofilename}_runtime_sample_trace.json`
2. `{videofilename}_stabilize_state.json`

Contents:
1. Runtime sample points over playback/scrub (`source_frame -> x,y,box,resolved`).
2. Face Stabilize on/off and binding status.
3. Keyframe usage stats and gaps.

## Error Artefacts (Always)
On failure at any stage, write:
1. `{videofilename}_error_<stage>.json`
2. `{videofilename}_error_<stage>.txt`

`error_<stage>.json` fields:
1. `run_id`
2. `stage`
3. `error_code`
4. `message`
5. `details`
6. `stack_or_process_output`
7. `timestamp_utc`

## UI Debug Surfaces

### A. Speaker Flow Debug Panel
Add a panel in Speakers tab:
1. `Enable Debug Capture` toggle.
2. `Open Latest Debug Run` button.
3. `Export Debug Bundle` button.
4. Stage list with status chips and elapsed ms.

### B. Inline Stage Status
Near existing status chips, show:
1. Current `run_id`.
2. Last failed stage (if any).
3. Quick action: `Open Error Artefact`.

### C. Assignment Dialog Diagnostics
In `Assign FaceStreams` dialog:
1. Show FaceStream track source used.
2. Show auto-match basis (timing overlap/nearest distance).
3. Show per-row validation reason for rejected manual IDs.
4. Show `Cluster` and `Track` columns so one person with multiple tracks is visible.
5. Show `Default Source` per row (`Persisted (Human)` or `Auto (Timing)`).
6. Provide explicit reset actions:
   - `Use Auto Suggestions`
   - `Use Persisted Mapping`
   - `Split Cluster`
   - `Merge Selected Clusters`
7. Applying assignments is explicit; no background remap occurs without user confirmation.
8. Assignment is optional for FaceStream generation, but required for transcript-speaker-specific tracking.

## Simplified UX Flow (Current)
1. User clicks `Generate FaceStream`.
2. System generates identity-agnostic continuity FaceStreams for each track (`T<track_id>`).
3. User clicks `Assign FaceStreams`.
4. System extracts one representative crop per generated FaceStream track.
5. System clusters tracks that likely belong to the same person.
6. System loads persisted resolved mappings for this clip (if present) and marks their source explicitly.
7. User assigns identity clusters to transcript speaker IDs and may split/merge clusters before applying.
8. System persists:
   - immutable machine run output
   - immutable machine cluster output
   - human override run output
   - resolved current mapping (`track_id -> speaker_id`) when assignment is used
9. Runtime tracking uses the resolved mapping to bind transcript speakers to the corresponding FaceStream tracks.

## Source Of Truth Contract
1. `speaker_flow.clips.<clip_id>.machine_runs.<run_id>`:
   - immutable machine tracks, representative crops, clusters, and suggestions.
2. `speaker_flow.clips.<clip_id>.human_runs.<run_id>`:
   - human cluster split/merge actions, assignment rows, override provenance, and audit log.
3. `speaker_flow.clips.<clip_id>.resolved_current`:
   - authoritative `track_id -> speaker_id` mapping used by speaker-aware tracking and stabilization.
4. Downstream steps must not infer identity directly from raw machine candidates if resolved mapping exists.
5. Continuity FaceStreams are authoritative for motion, independent of identity mapping presence.

## Overwrite Protection (Required)
If a stage is about to overwrite one or more existing artefacts in the target run folder:
1. Detect the full overwrite set before writing.
2. Prompt the user with a confirmation dialog listing:
   - stage name
   - number of files to overwrite
   - relative file names (or first N + “and X more”)
3. Provide actions:
   - `Overwrite`
   - `Cancel`
   - `Create New Run Instead` (recommended default)
4. If overwrite is approved, record the decision in:
   - `{videofilename}_overwrite_decision.json`
   - include `stage`, `files`, `approved_by_user`, `timestamp_utc`.

## Logging Contract
1. All generator and assignment stages write structured JSON + plain text logs.
2. C++ stages write both UI-visible messages and persisted logs.
3. Do not rely only on console output.

## Retention Policy
1. Keep latest 20 runs per clip by default.
2. Keep all runs marked `error` for 14 days minimum.
3. `Export Debug Bundle` always includes full run folder + index.

## Privacy / Safety
1. Artefacts may contain face crops and transcript text.
2. Debug capture should be explicit opt-in (default off in production UX).
3. Add `Redact Text` option for exported bundles when sharing externally.

## Implementation Plan

### Phase 1 (Foundational)
1. Add run manager + `index.json` writer.
2. Persist Stage 3 artefacts for FaceStream generation.
3. Persist Stage 4/5/6 artefacts for `Assign FaceStreams` flow.
4. Extract representative crops directly from generated FaceStream track keyframes.
5. Emit FaceStream track output (`tracks` + representative `candidates` + identity `clusters`).
6. Persist resolved current mapping as `track_id -> speaker_id`; clusters are review/grouping aids, not runtime motion sources.

### Phase 2 (UX)
1. Add Speakers debug panel with latest-run open/export.
2. Add inline stage status chips and run ID.
3. Add clustering review controls and assignment dialog diagnostic columns/tooltips.

### Phase 3 (Runtime Observability)
1. Add runtime sampling trace output (Stage 7).
2. Add profiling counters snapshot per run.
3. Add failure classifier for common recovery hints.

## Done Criteria
1. Any failed speaker flow operation yields an inspectable run folder.
2. A developer can determine root cause from artefacts without reproducing interactively.
3. A QA user can export a complete bundle from UI in under 3 clicks.
4. Schemas are versioned and validated in CI smoke tests.

## Suggested Next Task Breakdown
1. Implement `SpeakerFlowDebugRun` utility in C++ (run lifecycle + index writer).
2. Integrate it into FaceStream assignment and generation actions.
3. Add identity cluster artefacts using representative crops and visual embeddings.
4. Add artefact writes for FaceStream crop extraction and generation request/response logs.
5. Add minimal Speakers debug panel UI controls and wiring.
