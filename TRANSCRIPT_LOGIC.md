# Transcript / Speech Filter / Speaker Logic Review

## Latest Follow-Sync Decisions (April 24, 2026)
- Follow/highlight now uses **source timing only** in the transcript table; render-time columns were removed from the UI to avoid mixed timing semantics.
- Playhead-to-source mapping for transcript follow now includes render-sync markers and only runs when playhead is within the selected clip span.
- Manual row-selection hold no longer blocks follow after playback starts:
  - while paused, manual selection is still briefly honored;
  - once playback sample advances, follow resumes immediately.
- Transcript inspector controls were simplified to a single `Follow Current Word` checkbox that handles row highlight + auto-scroll behavior in the table.

## Scope
This review covers transcript loading/editing, Speech Filter range derivation, and speaker-related UI/data flows.

Primary files inspected:
- `transcript_tab.cpp`
- `transcript_engine.cpp`
- `editor_tabs.cpp`
- `editor.cpp`
- `editor_inspector_bindings.cpp`
- `editor_shared_transcript.cpp`
- `transform_skip_aware_timing.cpp`
- `render_decode.cpp`
- `preview_window_transcript.cpp`

## Current Logic

### Transcript Source Selection and Cuts
- Transcript path helpers:
  - Original: `<clipBase>.json` via `transcriptPathForClipFile(...)`.
  - Default editable: `<clipBase>_editable.json` via `transcriptEditablePathForClipFile(...)`.
  - Working fallback: editable if it exists, otherwise original, via `transcriptWorkingPathForClipFile(...)`.
- `TranscriptTab::loadTranscriptFile(...)` loads the selected cut path from the cut-version combo when present, otherwise default editable path.
- Cut versions are enumerated in `scriptVersionPathsForClip(...)` as:
  - original,
  - base editable,
  - `*_editable_vN.json` versions.
- Mutability is path-based: only non-original cuts are mutable (`activeCutMutable()`).

### Transcript Table + Editing
- Rows are parsed from JSON `segments[].words[]` in `parseTranscriptRows(...)`.
- Per-row speaker uses precedence:
  - word-level `speaker`,
  - segment-level `speaker`,
  - fallback `"Unknown"`.
- Row ordering is render-order-aware:
  - `render_order` first (if present),
  - then source start frame,
  - then insertion fallback order.
- Editable operations on mutable cuts:
  - time/text edit (`applyTableEdit(...)`),
  - skip/unskip (`setSelectedRowsSkipped(...)`),
  - delete (`deleteSelectedRows(...)`),
  - add word (`insertWordAtRow(...)`),
  - expand timing (`expandSelectedRow(...)`),
  - drag reorder persisted to `render_order` (`persistRenderOrderFromTable(...)`).

### Speech Filter Range Derivation
- Effective ranges are computed in `EditorWindow::effectivePlaybackRanges()`.
- If Speech Filter is enabled:
  - `TranscriptEngine::transcriptWordExportRanges(...)` maps transcript word time windows (with prepend/postpend) into timeline frames using clip timing + render-sync markers.
  - Skipped words are excluded (`word.skipped == true`).
- Result ranges feed:
  - playback stepping,
  - audio engine export ranges,
  - preview export ranges.

### Speaker UI and Metadata
- Transcript speaker filter combo (`All Speakers` or a concrete speaker id) only filters the table view (`filteredRowsForSpeaker(...)`).
- Speaker metadata table (`speaker_profiles`) allows editing:
  - display name,
  - normalized screen location `{x, y}`.
- Metadata persists into transcript JSON root at `speaker_profiles`.

## Findings (Priority Ordered)

### 1. Active cut selection is not honored by Speech Filter/overlay/render consumers
Severity: High

Observed behavior:
- `TranscriptTab` can load and edit `*_editable_vN.json` cut versions.
- But major downstream consumers resolve transcript path via `transcriptWorkingPathForClipFile(...)` (base editable/original), not selected cut path.

Evidence:
- Speech Filter: `TranscriptEngine::transcriptPathForClip(...)` -> `transcriptWorkingPathForClipFile(...)`.
- Preview transcript overlay: `PreviewWindow::transcriptSectionsForClip(...)` loads from working path.
- Render transcript overlay: `transcriptOverlayLayoutForFrame(...)` loads from working path.
- Transform skip-aware fallback transcript ranges: `interpolationFactorForTransformFrames(...)` loads from working path.

Impact:
- UI can show/edit one cut while playback/render/overlay still use another transcript file.
- User-visible mismatch is likely when working with versioned cuts.

Recommendation:
- Introduce one authoritative “active transcript path per clip” model and pass that to all transcript consumers (engine, preview, render, transform timing).
- Remove hidden fallback to working path for runtime decisions once active cut is known.

### 2. Loaded prepend/postpend values can desync between Editor and TranscriptTab
Severity: High

Observed behavior:
- `EditorWindow` stores `m_transcriptPrependMs/m_transcriptPostpendMs` and sets spinboxes during `loadState()` using `QSignalBlocker`.
- `TranscriptTab` has separate `m_transcriptPrependMs/m_transcriptPostpendMs` that are only updated in slot handlers (`onPrependMsChanged`, `onPostpendMsChanged`).
- Because load uses blocked signals, `TranscriptTab` can keep defaults (150/70) while `EditorWindow` uses loaded values.

Impact:
- Table source/render timing representation and derived row parsing in Transcript tab may not match active Speech Filter routing after project load.

Recommendation:
- Use a single source of truth for these parameters.
- At minimum, after load, explicitly push values into `TranscriptTab` state (without relying on valueChanged signals).

### 3. Transform skip-aware ranges are not refreshed on transcript document edits
Severity: Medium

Observed behavior:
- Global transform ranges are updated in:
  - `loadState()` and
  - `setupSpeechFilterControls()` refresh lambda.
- On transcript content changes (`transcriptDocumentChanged`), editor updates preview/audio export ranges but does not call `setTransformSkipAwareTimelineRanges(...)`.

Impact:
- `transformSkipAwareTiming` interpolation can continue using stale effective ranges after transcript edits (skip/delete/insert/etc.) until a speech-filter control changes.

Recommendation:
- In the `transcriptDocumentChanged` handler, recompute ranges and call `setTransformSkipAwareTimelineRanges(speechFilterPlaybackEnabled() ? ranges : {})`.

### 4. “All words skipped” in a transcripted clip falls back to passthrough audio
Severity: Medium

Observed behavior:
- In `transcriptWordExportRanges(...)`, if `sourceWordRanges` is empty for a clip, the clip is skipped from “filteredClipCoverage”.
- Later, passthrough ranges are computed as `base - filteredClipCoverage`.
- Therefore, a clip with a valid transcript but zero non-skipped words is treated as unfiltered passthrough.

Impact:
- Skipping all words does not mute/remove that clip from Speech Filter output.

Recommendation:
- Distinguish “transcript unavailable/invalid” from “transcript available with zero kept words”.
- For the latter, include clip in filtered coverage but contribute zero speech ranges.

### 5. Potential off-by-one when base ranges are empty
Severity: Low

Observed behavior:
- When `baseRanges` is empty, `endFrame` defaults to `clip.startFrame + clip.durationFrames`.
- Clip coverage elsewhere uses `clip.startFrame + clip.durationFrames - 1`.

Impact:
- One extra frame can be included in full-range fallback mode.

Recommendation:
- Align fallback to inclusive frame convention: `start + durationFrames - 1`.

### 6. Speaker metadata is persisted but not consumed by runtime rendering
Severity: Low

Observed behavior:
- `speaker_profiles` name/location is editable and saved.
- No consumer in preview/render logic reads speaker profile name/location.

Impact:
- Current speaker profile editing is effectively metadata-only from runtime perspective.

Recommendation:
- Either wire these fields into transcript/title overlay placement logic, or label the feature explicitly as metadata-only.

## Additional Notes
- Speaker filter is a view filter only; Speech Filter logic is not speaker-aware.
- Drag-to-reorder is intentionally disabled unless:
  - active cut is mutable,
  - speaker filter is `All Speakers`,
  - “Show Lines Not In Active Cut” is off.
- Caching in `TranscriptEngine` is signature-based (includes transcript mtime, clip timing, markers, prepend/postpend) and is invalidated on transcript/speech-parameter signals.

## Suggested Fix Order
1. Unify active cut path across all transcript consumers (Speech Filter, preview, render, transform timing).
2. Remove parameter state duplication between `EditorWindow` and `TranscriptTab` for prepend/postpend/fade/enabled.
3. Refresh transform skip-aware timeline ranges on `transcriptDocumentChanged`.
4. Correct “all-skipped words” behavior to produce silence/removal for transcripted clips.
5. Fix fallback range off-by-one.
6. Decide product intent for `speaker_profiles` runtime usage and implement/label accordingly.

## Integration Test Policy (Current)
- Default CI path is stability-first:
  - Real media generation + metadata ingest check for MP4.
  - Async decode runthrough uses generated image sequences (robust across environments).
- Optional packet decode gate is available in `test_integration`:
  - `testDecodeRealVideoPacketPathOptional`.
  - Enable with env var: `JCUT_ENABLE_UNSTABLE_PACKET_DECODE_TEST=1`.
- This keeps routine test runs reliable while preserving an explicit real packet-decode validation path when needed.
