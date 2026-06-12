# TODO

## Current State

- FaceDetections sidecars have been migrated away from large inline JSON payloads.
- `_facedetections.bin` and `_facedetections_processed.bin` are compact binary sidecars that reference external indexed artifacts.
- External `tracks.idx`/`tracks.dat` and `detections.idx`/`detections.dat` are the authoritative large FaceDetections payloads.
- UI paths now intentionally avoid materializing full external raw track artifacts.
- The running preview can be responsive, but continuity boxes are not visible when the sidecar contains only compact external raw artifact references and no stored `streams`.

## Why Continuity Tracks Are Not Visible

- Preview overlays call `storedContinuityStreamsForRoot(...)`.
- `storedContinuityStreamsForRoot(...)` only returns already-materialized `streams` or small stored stream artifacts.
- The current migrated sidecars point at external raw indexed artifacts and do not include stored UI-ready streams.
- Full derivation through `continuityStreamsForRoot(...)` is intentionally not used by preview/Speakers UI paths because it expands all raw tracks, freezes the UI, and can allocate tens of GB.

## Professional Boundary

- Keep UI paths bounded.
- Do not reintroduce full raw-track expansion on the UI thread.
- Keep external artifacts usable through explicit bounded readers.
- Use unbounded full derivation only for intentional offline jobs, migration tools, or tests.

## Required Next Work

1. Keep the bounded current-playhead continuity reader on external `tracks.idx`.
2. Use that reader in preview overlays so only tracks relevant to the current frame/window are decoded.
3. Use the same bounded reader for the Speakers playhead candidate list.
4. Preserve selective assignment reads through `continuityStreamsForAssignments(...)`.
5. Add regression tests proving preview/Speakers UI paths never call full external raw-track derivation.

## Recommended Implementation

- Keep the lightweight record index in `tracks.idx`.
- Store per-track metadata without expanding all keyframes:
  - `track_id`
  - `stream_id`
  - min/max frame
  - record offset
  - compressed record size
  - optional sparse frame samples
- Save the index next to the artifact, or embed it in the compact sidecar if small.
- For each preview refresh:
  - determine the active clip and source frame
  - find only tracks whose min/max frame spans the current frame plus edge-hold window
  - decode only those track records
  - resolve the nearest keyframe/box for display

## Acceptance Criteria

- Continuity boxes are visible during face assignment from compact migrated artifacts.
- Clicking a face remains responsive.
- Preview live profile stays responsive while playback is active.
- Opening the FaceDetections paths panel does not load all records.
- RSS does not jump by multiple GB when toggling track visibility or clicking faces.
- Tests cover:
  - compact sidecar with external `tracks.idx`/`tracks.dat`
  - bounded preview/playhead lookup
  - selective assignment lookup
  - no UI-path call to full `continuityStreamsForRoot(...)` for external raw artifacts

## Current Verification Commands

```bash
cmake --build build --target jcut test_facedetections_processed_artifact -j4
ctest --test-dir build --output-on-failure -R "test_facedetections_processed_artifact|test_facedetections_artifacts|test_transcript_logic|test_transcript_tab_follow"
git diff --check
```

---

> Historical note (2026-06-11): the TODO.md path previously held the "Vulkan Preview Black
> Output" debugging evidence and ladder. That content is preserved in substance in
> ambitious_plan.md Appendix A6 (and verbatim in git history at 414b765) and drives the plan's
> Phase 0 diagnosis.
