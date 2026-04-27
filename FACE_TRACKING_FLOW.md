# Face Tracking Flow (Expected User Path)

This is the intended end-to-end flow starting from a clip that has both video and audio.

## 1. Start With a Source Clip
1. Import/select a clip that includes both video and audio.
2. Confirm the clip is selected in the timeline.

## 2. Create/Select an Editable Transcript Cut
1. Open the `Transcript` tab.
2. Create a transcript cut version (or select an existing non-original editable cut).
3. Keep this editable cut active for all speaker and face operations.

## 3. Transcribe
1. Run transcription for the selected clip/cut.
2. Wait until transcript segments/words and speaker IDs are present.

## 4. Mine Transcript (AI)
1. Open the `Speakers` tab.
2. Click `Mine Transcript (AI)` to suggest better speaker names.
3. Optionally run `Find Organizations` and `Clean Assignments`.
4. Apply accepted suggestions so speaker profiles are populated.

## 5. ID Faces (Candidate Discovery)
1. In `Speakers`, click `Pre-crop Faces`.
2. The app samples frames, detects potential faces, and generates crop candidates.
3. In the assignment dialog, use auto-preselected speaker matches (preferred route).

## 6. Match Faces to Speaker IDs
1. Review each candidate crop row.
2. Preferred: keep `Assign To (Auto)` when correct.
3. Backup: enter `Manual Speaker ID (Override)` when auto is wrong or missing.
4. Apply assignments.
5. Result: assigned faces fill empty `Ref1`/`Ref2` slots for each speaker.

## 7. Fill Missing References (If Needed)
1. If a speaker still lacks references, use:
2. `Pick Ref 1 (Shift+Drag)` / `Pick Ref 2 (Shift+Drag)` in Preview, or
3. `Set Ref 1` / `Set Ref 2` at the current playhead frame.

## 8. Generate BoxStream (Tracking Data)
1. Select a speaker row with at least `Ref1` set.
2. Click `Generate BoxStream`.
3. Optional: set both refs first for stronger tracking quality, then regenerate.

## 9. Enable Speaker Tracking
1. Turn on `Tracking` for the speaker (Tracking chip/button).
2. Confirm the state shows BoxStream ready/active.

## 10. Set FaceBox Target
1. In `Speakers`, use `FaceBox (Yellow Box)` controls:
2. Set `Yellow Box X`, `Yellow Box Y`, and optional `Yellow Box Size`.
3. Toggle `Show FaceBox` to visually confirm target placement in Preview.

## 11. Enable Face Stabilize (Clip-Level)
1. Enable `Face Stabilize` for the selected clip.
2. Confirm status updates (`Face Stabilize: ON` and runtime/keys state).

## 12. Validate and Iterate
1. Scrub and play through speaker changes.
2. If tracking drifts:
3. Add/adjust refs, regenerate BoxStream, and re-check target placement.
4. Re-run `Pre-crop Faces` when new candidates are needed.

## 13. Render
1. Once tracking and framing behavior are stable, proceed to render/export.

## Notes
- Preferred UX route: `Pre-crop Faces` auto-assignment first.
- Manual fallback is always available via `Manual Speaker ID (Override)` and direct ref picking.
- Speaker/face edits are intended for editable transcript cuts, not immutable original transcripts.
