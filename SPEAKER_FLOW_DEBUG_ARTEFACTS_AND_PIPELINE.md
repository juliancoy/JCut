# Speaker Flow Debug Artefacts And Pipeline

## Goal
Provide a deterministic, stage-by-stage debug pipeline for the Speaker Flow so failures are diagnosable from one place without reading source code.

## Scope
Covers this end-to-end path:
1. Transcript ingestion and speaker IDs
2. AI name mining and speaker profile updates
3. Face candidate detection (`Pre-crop Faces`)
4. Candidate-to-speaker assignment (auto + manual override)
5. Reference writes (`Ref1`/`Ref2`)
6. BoxStream generation (`Generate BoxStream`)
7. Runtime tracking and Face Stabilize application

## Principles
1. Every stage writes a machine-readable artefact.
2. Every artefact has stable schema + version.
3. Every run has a single `run_id` that links all artefacts.
4. Failures are first-class: write error artefacts, not only logs.
5. UI exposes links to current run artefacts.

## Proposed Debug Artefact Root
`projects/<project_id>/debug/speaker_flow/<clip_id>/<run_id>/`

Where `run_id` format is:
`YYYYMMDD-HHMMSS-<short_uuid>`

## Artefact Naming Convention (Required)
All artefacts must use:
`{videofilename}_artefacttype.ext`

Examples:
1. `interview_take_03_transcript_snapshot.json`
2. `interview_take_03_face_detection_output.json`
3. `interview_take_03_boxstream_log.txt`

Rules:
1. `videofilename` is the source clip basename without extension, sanitized to `[a-zA-Z0-9._-]`.
2. `artefacttype` is a stable snake_case token from this plan.
3. If multiple files of same type are required, append `_partN` or `_idxN` before extension.

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
Files:
1. `{videofilename}_transcript_snapshot.json`
2. `{videofilename}_speaker_ids.json`

Contents:
1. Active transcript/cut metadata.
2. Speaker IDs discovered from segments/words.
3. Speaker word counts and timing spans.

### Stage 2: AI Name Mining
Files:
1. `{videofilename}_name_mining_input.json`
2. `{videofilename}_name_mining_output.json`
3. `{videofilename}_name_mining_apply.json`

Contents:
1. Candidate names + confidence/rationale per speaker.
2. Which names were auto-applied.
3. Which existing-name overwrites were user-approved.
4. Which suggestions were rejected.

### Stage 3: Face Candidate Detection
Files:
1. `{videofilename}_face_detection_request.json`
2. `{videofilename}_face_detection_output.json`
3. `{videofilename}_face_detection_log.txt`
4. `{videofilename}_face_crops/` (pngs)

Contents:
1. Detector params (`step`, `max_candidates`, fps, frame range).
2. Candidate list (`frame`, `x`, `y`, `box`, `score`, crop path).
3. Raw process stdout/stderr.

### Stage 4: Candidate Assignment
Files:
1. `{videofilename}_assignment_table.json`
2. `{videofilename}_assignment_decisions.json`

Contents:
1. Auto suggestion per candidate.
2. Manual override value if provided.
3. Validation outcome (accepted/rejected + reason).
4. Final resolved speaker ID per accepted candidate.

### Stage 5: Reference Write
Files:
1. `{videofilename}_reference_write_plan.json`
2. `{videofilename}_reference_write_result.json`

Contents:
1. Free slot availability (`ref1`, `ref2`) before write.
2. Chosen candidate per slot.
3. Write result per speaker/slot (`ok|blocked|error`).
4. Updated `framing` summary.

### Stage 6: BoxStream Generation
Files:
1. `{videofilename}_boxstream_request.json`
2. `{videofilename}_boxstream_output_keyframes.json`
3. `{videofilename}_boxstream_stats.json`
4. `{videofilename}_boxstream_log.txt`

Contents:
1. Engine path used (native/docker/linear fallback/anchor fallback).
2. Reference inputs and speaker windows.
3. Keyframe count, frame coverage, confidence summary (if available).
4. Any fallback reason and error traces.

### Stage 7: Runtime Sampling / Face Stabilize
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
In `Pre-crop Faces` dialog:
1. Show detector params used.
2. Show auto-match basis (timing overlap/nearest distance).
3. Show per-row validation reason for rejected manual IDs.

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
1. All scripts write structured JSON + plain text logs.
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
2. Persist Stage 3/4/5 artefacts for `Pre-crop Faces` flow.
3. Persist Stage 6 artefacts for BoxStream generation.

### Phase 2 (UX)
1. Add Speakers debug panel with latest-run open/export.
2. Add inline stage status chips and run ID.
3. Add assignment dialog diagnostic columns/tooltips.

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
2. Integrate it into `onSpeakerPrecropFacesClicked` and BoxStream actions.
3. Add artefact writes in `speaker_face_candidates.py` and `speaker_boxstream.py` (request/response logs).
4. Add minimal Speakers debug panel UI controls and wiring.
