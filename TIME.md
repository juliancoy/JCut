# TIME.md

## Purpose
This document maps the temporal domains in JCut, the conversion paths between them, and the practical implications for playback, transcript sync, speech filtering, overlays, and transforms.

## Canonical Temporal Domains
- `absolutePlaybackSample` (48 kHz timeline sample domain): main runtime playhead state in `EditorWindow`.
- Timeline frame domain (`kTimelineFps = 30`): UI timeline, clip placement, export ranges, render frame loop.
- Clip-local timeline frame/sample domain: position inside a clip after subtracting clip start.
- Source frame/sample domain: decoded media position after clip trim, playback rate, and render-sync markers.
- Transcript frame domain (`30 fps` transcript clock): transcript words/sections and follow/highlight matching.
- Filtered timeline domain (speech ranges only): sparse timeline intervals from transcript words.

## Core Conversion Paths
- Timeline sample -> timeline frame:
  - `samplesToFramePosition(...)`
- Timeline frame/sample -> source frame/sample:
  - `sourceFrameForClipAtTimelinePosition(...)`
  - `sourceSampleForClipAtTimelineSample(...)`
  - Applies render-sync marker deltas via `adjustedClipLocalFrameAtTimelineFrame(...)`.
- Timeline sample -> transcript frame:
  - `transcriptFrameForClipAtTimelineSample(...)`
  - Converts source sample to source seconds, then to transcript frame.
- Transcript word times (seconds) -> transcript frames:
  - `TranscriptTab::parseTranscriptRows(...)`
  - `TranscriptEngine::transcriptWordExportRanges(...)`

## Runtime Clock Paths
- Audio-master path:
  - `EditorWindow::advanceFrame()` uses `AudioEngine::currentSample()` when `shouldUseAudioMasterClock(...)` is true.
  - Implication: visual playhead follows audio clock; best A/V lock when audio is healthy.
- Timeline-timer path:
  - `advanceFrame()` increments by elapsed wall-clock * playback speed.
  - Implication: fallback when audio unavailable/stalled; can diverge from audio if audio later resumes.
- Clock policy controls:
  - `PlaybackClockSource`: `auto|audio|timeline`.
  - `PlaybackAudioWarpMode`: `disabled|varispeed|time_stretch`.
  - Normalization can force `disabled -> varispeed` at non-1.0 speed.

## Transcript / Speech Filter Paths
- Source of transcript truth per clip:
  - `activeTranscriptPathForClipFile(...)`.
  - Falls back to editable/original if no active override.
- Transcript follow path:
  - `EditorWindow::syncTranscriptTableToPlayhead()` computes source frame from playhead sample.
  - `TranscriptTab::syncTableToPlayhead(...)` matches by source-frame ranges.
  - Implication: follow/highlight should align with click-to-seek because both use source-derived timing.
- Speech filter range path:
  - `effectivePlaybackRanges()` -> `TranscriptEngine::transcriptWordExportRanges(...)`.
  - Non-skipped transcript words (+ prepend/postpend) become kept timeline ranges.
  - If speech filter enabled, playback stepping uses these sparse ranges.
- Filtered sample projection:
  - `filteredPlaybackSampleForAbsoluteSample(...)` maps absolute playhead to condensed filtered time.

## Render / Preview Transcript Overlay Paths
- Preview overlay timing:
  - `PreviewWindow::transcriptOverlayLayoutForClip(...)` uses `transcriptFrameForClipAtTimelineSample(...)`.
- Render overlay timing:
  - `render_decode.cpp::transcriptOverlayLayoutForFrame(...)` uses same transcript-frame mapping.
- Implication:
  - Preview and export overlay word activation are on the same source-derived temporal path.

## Transform Skip-Aware Timing Paths
- Primary path (preferred):
  - `setTransformSkipAwareTimelineRanges(...)` provides active speech-kept timeline ranges.
  - `interpolationFactorForTransformFrames(...)` remaps interpolation progress to effective visible timeline duration.
- Fallback path:
  - If no global ranges are set, loads transcript speech ranges from active transcript path and derives effective duration in source-frame space.
- Implication:
  - Keyframed transforms can progress over spoken content only, not silent/removed spans.

## Speech Boundary Fade/Crossfade Paths
- Speech range gain shaping in audio mix:
  - `calculateSpeechRangeBlend(...)` in `AudioEngine`.
  - Modes: edge fades only or boundary crossfade between adjacent speech ranges.
- Clip edge gain shaping:
  - `calculateClipCrossfadeGain(...)` applies clip fade at clip boundaries.
- Implication:
  - Speech filter timing determines where audio is audible; fade/crossfade changes transition smoothness without changing timeline duration.

## Speaker Framing Temporal Path
- Active word/speaker resolution uses source frame inside transcript sections.
- `transcriptSpeakerLocationForSourceFrame(...)` interpolates framing keyframes in source-frame time.
- Preview/render transcript box translation can follow that resolved position.
- Implication:
  - Speaker position lock is tied to transcript/source time, not UI frame index alone.

## Persistence and API Control Paths
- Persisted playback temporal config:
  - speed, clock source, warp mode.
- REST patch path:
  - `applyPlaybackConfigPatch(...)` for playback clock/warp/speed.
  - `applyThrottleConfigPatch(...)` for timing-related throttles (UI sync interval, selection hold, stall thresholds, speaker tracking smoothing/speed).
- Implication:
  - Temporal behavior is externally tunable; profiling snapshots expose runtime timing/latency counters.

## Known High-Impact Implications
- Mixed domains are the main drift risk.
  - Safe pattern: convert playhead -> source sample/frame once, then reuse that for transcript/follow/overlay/speaker logic.
- Render-sync markers are temporal modifiers.
  - Any consumer bypassing marker-adjusted mapping can desync from playback.
- Speech-filtered playback is sparse-time traversal.
  - UI or transform systems assuming continuous timeline time can feel “fast” or “jumping” unless they use filtered/effective duration paths.
- Audio clock stability governs perceived sync quality in playback.
  - Stalls trigger fallback to timer path; repeated transitions can present jitter if decode/audio is under pressure.

## Practical Source-of-Truth Rules
- Primary runtime playhead truth: `absolutePlaybackSample`.
- Transcript truth at a playhead instant: marker-aware `sourceSample/sourceFrame` derived from that sample.
- Speech keep/remove truth: `TranscriptEngine::transcriptWordExportRanges(...)` from active transcript path.
- Overlay/speaker timing truth: transcript/source frame mapping, not render-order table position.

## Temporal Invariants
- Invariant 1: Follow/highlight selection decisions must be made in source-frame space, never render-row order or render-time columns.
- Invariant 2: Any playhead-based transcript consumer must derive source timing through marker-aware conversion (`sourceSampleForClipAtTimelineSample(...)` / `transcriptFrameForClipAtTimelineSample(...)`).
- Invariant 3: When speech filter is enabled, active playback traversal must use `effectivePlaybackRanges()` output; unfiltered stepping is invalid in that mode.
- Invariant 4: Preview transcript overlay and render transcript overlay must use the same transcript-frame mapping path to avoid WYSIWYG drift.
  - Enforced via shared helper: `transcriptOverlayLayoutAtSourceFrame(...)`.
- Invariant 5: Active transcript cut path (`activeTranscriptPathForClipFile(...)`) is the transcript source of truth for runtime consumers.
- Invariant 6: Skip-aware transform interpolation must use either active global kept ranges (`setTransformSkipAwareTimelineRanges(...)`) or active transcript speech ranges; stale range state is invalid.

## Verification Matrix
- Playhead clock policy and warp normalization:
  - Code: `editor_shared_media.cpp`, `editor.cpp`
  - Tests: `tests/test_playback_policy.cpp`
- Timeline sample -> source/transcript mapping:
  - Code: `editor_shared_render_sync.cpp`
  - Tests: `tests/test_transcript_logic.cpp::testTranscriptFrameMappingUsesSourceSeconds`
- Active transcript cut usage + all-skipped behavior:
  - Code: `transcript_engine.cpp`, `editor_shared_transcript.cpp`
  - Tests: `tests/test_transcript_logic.cpp::testSpeechFilterUsesActiveTranscriptCut`, `tests/test_transcript_logic.cpp::testAllSkippedWordsYieldNoSpeechRanges`
- Transcript follow stability (continuous, gap bridge, reverse, skipped rows, outside-cut behavior):
  - Code: `transcript_tab.cpp`, `editor.cpp`
  - Tests: `tests/test_transcript_tab_follow.cpp`
- Speaker framing temporal interpolation and fallback behavior:
  - Code: `editor_shared_transcript.cpp`, `preview_window_transcript.cpp`, `render_decode.cpp`
  - Tests: `tests/test_transcript_logic.cpp` speaker-tracking cases
- Shared preview/render transcript overlay layout path:
  - Code: `editor_shared_transcript.cpp`, `preview_window_transcript.cpp`, `render_decode.cpp`
  - Tests: `tests/test_transcript_logic.cpp::testTranscriptOverlayLayoutHelperMatchesSectionLayout`
- Transform skip-aware timing progression:
  - Code: `transform_skip_aware_timing.cpp`
  - Tests: `tests/test_transform_skip_aware_timing.cpp`

## Failure Playbook
- Symptom: clicking a transcript word seeks correctly, but follow highlight drifts during playback.
  - Check: ensure follow path is consuming source frame from `transcriptFrameForClipAtTimelineSample(...)`.
  - Probe: `/profiling` (`absolute_playback_sample`, playhead ages), then verify selected clip transcript path and marker set.
- Symptom: preview transcript overlay timing differs from export render.
  - Check: both preview and render must use transcript-frame mapping from timeline sample/frame plus render-sync markers.
  - Probe: compare preview clip/time against a rendered frame at same timeline frame.
- Symptom: speech filter “skip all words” still passes clip audio.
  - Check: `transcriptWordExportRanges(...)` output should be empty for that clip coverage.
  - Probe: inspect effective ranges via state/profiling and verify active transcript file content.
- Symptom: transforms animate during silent/skipped sections under skip-aware mode.
  - Check: whether `setTransformSkipAwareTimelineRanges(...)` was refreshed after transcript edits.
  - Probe: verify speech filter/timing ranges snapshot and transform interpolation inputs.
- Symptom: transcript follow or overlay breaks only when render-sync markers exist.
  - Check: any bypass of marker-aware conversion helpers.
  - Probe: validate marker list and compare mapped source frame with/without markers for same timeline point.

## Operational Checklist
- Before merging timing changes:
  - Run `ctest --test-dir build --output-on-failure`.
  - Run transcript follow tests and playback policy tests specifically.
  - Validate one manual scenario with render-sync markers and speech filter enabled.
- Before release:
  - Capture `/profiling` snapshots at idle and during playback.
  - Confirm preview/export transcript alignment at at least one known timestamp.
