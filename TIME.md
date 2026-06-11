# TIME.md

## Purpose
This document maps the temporal domains in JCut, the conversion paths between them, and the practical implications for playback, transcript sync, speech filtering, overlays, and transforms.

## Canonical Temporal Domains
- `absolutePlaybackSample` (48 kHz timeline sample domain): main runtime playhead state in `EditorWindow`.
- Timeline/edit frame domain (`kTimelineFps = 30`): UI timeline, clip placement, user-facing edit keyframes, export range bookkeeping, and the fixed-rate output render loop.
- Clip-local timeline frame/sample domain: position inside a clip after subtracting clip start.
- Source frame/sample domain: decoded media position after clip trim, playback rate, source FPS, and render-sync markers.
- Media transfer/decode frame domain: the actual decoded source frame index requested from the media pipeline. It is not a 30 fps clock and must not inherit `kTimelineFps` except as an explicit fallback when media FPS is unknown.
- Transcript frame domain (`30 fps` transcript clock): transcript words/sections and follow/highlight matching.
- Filtered timeline domain (speech ranges only): sparse timeline intervals from transcript words.

## Facestream Frame-Domain Classes (Time-Relative)
- `FacestreamFrameDomain::ClipTimeline30Fps`
  - Historical/manual facestream domain only. It is not valid for generated FaceDetections artifacts.
  - FaceDetections readers must reject or ignore this domain instead of remapping through `kTimelineFps`.
- `FacestreamFrameDomain::SourceRelative`
  - `keyframe.frame` is source frame relative to `clip.sourceInFrame` (local source offset).
  - Source mapping: `clip.sourceInFrame + keyframe.frame`.
  - Lookup basis while previewing: current local source frame (`currentSourceFrame - clip.sourceInFrame`).
- `FacestreamFrameDomain::SourceAbsolute`
  - `keyframe.frame` is absolute source frame index in media decode space.
  - Source mapping: direct `sourceFrame = keyframe.frame`.
  - Lookup basis while previewing: current absolute source frame.

## Facestream Domain Inference Rules
- Inference entry point: `inferFacestreamFrameDomain(...)`.
- Preferred classification order:
  - `SourceAbsolute` when keyframe frame range falls inside clip source absolute range (`clip.sourceInFrame .. clip.sourceInFrame + clip.sourceDurationFrames`, tolerant bounds).
  - Else fallback to `SourceRelative`.
- Important implication:
  - A wrong domain classification creates deterministic time drift even if box geometry is correct.
  - Once a facestream keyframe is mapped to source-frame space, decoder/avatar paths must use that source frame directly. Re-scaling the mapped source frame through `kTimelineFps` is invalid.
  - Generated FaceDetections artifacts must declare and consume only source media domains (`source_absolute` or `source_relative`).

## Core Conversion Paths
- Timeline sample -> timeline frame:
  - `samplesToFramePosition(...)`
- Timeline frame/sample -> source frame/sample:
  - `sourceFrameForClipAtTimelinePosition(...)`
  - `sourceSampleForClipAtTimelineSample(...)`
  - Applies render-sync marker deltas via `adjustedClipLocalFrameAtTimelineFrame(...)`.
- Facestream stored frame -> source frame:
  - `mapFacestreamFrameToSourceFrame(...)`
  - Applies the stream's declared/inferred `FacestreamFrameDomain`.
  - Output is a source/decode frame, not another timeline-30 frame.
- Timeline sample -> transcript frame:
  - `transcriptFrameForClipAtTimelineSample(...)`
  - Converts source sample to source seconds, then to transcript frame.
- Presented media source frame -> transcript frame:
  - `transcriptFrameForClipSourceFrame(...)`
  - Converts the actually presented decoded media frame to source seconds, then to transcript frame.
  - Live visual overlays that must stick to the displayed picture use this path when a presented frame is available.
- Transcript word times (seconds) -> transcript frames:
  - `TranscriptTab::parseTranscriptRows(...)`
  - `TranscriptEngine::transcriptWordExportRanges(...)`

## FaceDetections Click / Track Assignment Timing
- FaceDetections continuity track frames are media source frames.
  - `source_absolute` samples, preview box clicks, and track assignment anchors are in the clip's decoded media source-frame domain.
  - This domain uses `resolvedSourceFps(clip)`, not `kTimelineFps`.
- Transcript speaker lookup is in transcript-frame time.
  - Transcript JSON stores word/section times in seconds.
  - Runtime transcript frame lookup converts seconds with `kTimelineFps`.
  - Active speaker lookup, transcript section selection, and speaker gap-hold decisions must therefore use transcript frames.
- Required conversion for a FaceDetections click:
  - `mediaSourceFrame = clicked source frame`
  - `sourceSeconds = mediaSourceFrame / resolvedSourceFps(clip)`
  - `transcriptFrame = floor(sourceSeconds * kTimelineFps)`
- Required storage rule after assignment:
  - Preserve `mediaSourceFrame` for the FaceDetections track anchor and geometry paths.
  - Use `transcriptFrame` only to resolve the active transcript speaker/section.
- Required logging rule:
  - Any FaceDetections assignment or rejection log must name the domains explicitly: media source frame, transcript frame, source FPS, and whether gap hold was used.
- Explicit failure case:
  - Never compare a FaceDetections media source frame directly to transcript frames.
  - Example: on a 60 fps source, media source frame `258214` is about `4303.57s` and maps to transcript frame `129107` at 30 fps; treating `258214` as a transcript frame incorrectly means about `8607.13s`.

## Runtime Clock Paths
- Single-clock contract:
  - At any runtime instant, the application has one authoritative playhead sample in 48 kHz timeline-sample space.
  - `absolutePlaybackSample` is the canonical state stored by `EditorWindow`.
  - Every playback consumer must derive its local domain from that sample instead of advancing its own independent notion of now.
  - Derived domains include timeline frame, media source frame, transcript frame, retimed audio cache sample, speaker label timing, overlay timing, and follow/highlight timing.
- Audio-master path:
  - `EditorWindow::advanceFrame()` uses `AudioEngine::currentSample()` when `shouldUseAudioMasterClock(...)` is true.
  - `AudioEngine::currentSample()` is the effective audible output position in 48 kHz timeline-sample space, not the tail of already queued audio.
  - Practical definition: derive from queued end position minus still-buffered frames and output-stream latency.
  - Implication: visual playhead follows the sample reaching the output device as closely as backend timing permits; this is the best available A/V lock when audio is healthy.
- Timeline-timer path:
  - `advanceFrame()` increments by elapsed wall-clock * playback speed.
  - Implication: fallback only when audio is intentionally not the master or no playable audio exists.
  - It is invalid to use timer fallback to ride through a blocked pitch-preserving audio segment, because that creates a second runtime clock.
- Clock policy controls:
  - `PlaybackClockSource`: `auto|audio|timeline`.
  - `PlaybackAudioWarpMode`: `disabled|varispeed|time_stretch`.
  - Normalization can force `disabled -> varispeed` at non-1.0 speed.
- Pitch-preserving time-stretch audio:
  - In `time_stretch` mode at any non-1.0 playback speed, `AudioEngine` reads from pitch-preserved retimed cache entries when available.
  - Sidecar files are used for exact integer 200% and 300% speeds. Other speeds, including 125% and 150%, use generated in-memory segmented cache entries keyed by rounded speed-per-mille.
  - This does not create a new clock domain. `AudioEngine::currentSample()` still reports the effective audible position in 48 kHz timeline-sample space.
  - Timeline sample -> source sample still uses `sourceSampleForClipAtTimelineSample(...)` with render-sync markers and clip playback rate.
  - The retimed buffer index is derived from that mapped source sample by dividing by the global time-stretch speed.
  - Retimed cache entries may be segmented. Segment lookup must choose an entry that covers the entire mix chunk in normalized retimed sample space; using a stale earlier segment creates `input_out_of_range` stalls.
  - If the retimed buffer is not ready, the fallback is explicit: playback enters audio warmup/blocked state and increments `time_stretch_cache_miss_count`; it must not silently fall back to pitch-shifted varispeed audio.
  - While blocked, the audio clock is not a valid master clock for video. `EditorWindow::advanceFrame()` must call `requestPlaybackAudioWarmup(true)` and hold or snap back to the last effective audible timeline sample instead of letting video follow a stuck or rewound audio cursor.

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
  - Safe facestream pattern: convert stored keyframe frame -> source frame once, then decode/crop from that source frame directly.
- Render-sync markers are temporal modifiers.
  - Any consumer bypassing marker-adjusted mapping can desync from playback.
- Speech-filtered playback is sparse-time traversal.
  - UI or transform systems assuming continuous timeline time can feel “fast” or “jumping” unless they use filtered/effective duration paths.
- Audio clock stability governs perceived sync quality in playback.
  - Stalls trigger fallback to timer path; repeated transitions can present jitter if decode/audio is under pressure.
  - A clock derived from future queued/submitted position instead of effective audible position will make visual playheads lead the heard audio.
  - A blocked pitch-preserving audio segment is not an audio-clock stall to ride through; playback must gate until the needed retimed segment is available.
- Playback-rate stress exposes stale-overlay mistakes.
  - At 300% playback, overlays and captions may be visually less smooth if rendering drops work, but they must still sample the same canonical playhead/source frame as audio and video.
  - Reusing an old FaceDetections overlay is allowed only as a short presentation hold while a matching source-frame cache result is being prepared.
  - A preserved overlay must be close to the requested media source frame; it must not become a separate visual clock.

## Practical Source-of-Truth Rules
- Primary runtime playhead truth: `absolutePlaybackSample`.
- Derived domain rule: timeline frame, source frame/sample, transcript frame, retimed cache sample, speaker labels, captions, and overlays must all be derived from the active playhead sample through the canonical conversion helpers.
- Audio-master clock truth: effective audible output position, not future queued/submitted position.
- Pitch-preserving audio readiness truth: `AudioEngine::playbackAudioReadyForFrame(...)` and `AudioEngine::playbackAudioBlocked()` gate playback start/continuation at non-1.0 `time_stretch` speeds.
- Timer fallback truth: allowed only when audio is not the selected/effective master or there is no playable audio; invalid when pitch-preserving audio is required but blocked or not ready.
- Transcript truth at a playhead instant: marker-aware `sourceSample/sourceFrame` derived from that sample.
- Live subtitle overlay truth: the presented media source frame when a frame has already been selected for display; this prevents subtitles from visually leading late video.
- Speech keep/remove truth: `TranscriptEngine::transcriptWordExportRanges(...)` from active transcript path.
- Overlay/speaker timing truth: transcript/source frame mapping, not render-order table position.
- FaceDetections playback overlay truth: requested media source frame from the canonical playhead. Previous overlay reuse is a bounded display hold, not continuity evidence, and must be cleared when drift exceeds the small source-frame tolerance.
- Preview stale-frame tolerance: playback presentation may hold an approximate video frame only within the shared 4-source-frame ceiling; anything older must be dropped rather than displayed as current video.
- Debug truth: logs and REST fields that cross domains must name the domain explicitly (`timeline_sample`, `timeline_frame`, `media_source_frame`, `transcript_frame`, `retimed_cache_sample`) instead of using ambiguous `source_frame` labels.

## Temporal Invariants
- Invariant 1: Follow/highlight selection decisions must be made in source-frame space, never render-row order or render-time columns.
- Invariant 2: Any playhead-based transcript consumer must derive source timing through marker-aware conversion (`sourceSampleForClipAtTimelineSample(...)` / `transcriptFrameForClipAtTimelineSample(...)`).
- Invariant 3: When speech filter is enabled, active playback traversal must use `effectivePlaybackRanges()` output; unfiltered stepping is invalid in that mode.
- Invariant 4: Preview transcript overlay and render transcript overlay must use the same transcript-frame mapping path to avoid WYSIWYG drift.
  - Enforced via shared helper: `transcriptOverlayLayoutAtSourceFrame(...)`.
  - During live Vulkan presentation, the preview overlay must prefer the presented media-source-frame mapping (`transcriptFrameForClipSourceFrame(...)`) over the audio-clock playhead when a presented frame is available.
- Invariant 5: Active transcript cut path (`activeTranscriptPathForClipFile(...)`) is the transcript source of truth for runtime consumers.
- Invariant 6: Skip-aware transform interpolation must use either active global kept ranges (`setTransformSkipAwareTimelineRanges(...)`) or active transcript speech ranges; stale range state is invalid.
- Invariant 7: In pitch-preserving `time_stretch` playback, video must not follow an audio clock that is blocked on a missing or out-of-range retimed segment.
- Invariant 8: Any code path that stores or advances "current frame" during playback must derive it from `absolutePlaybackSample` or the valid audio-master sample used to update `absolutePlaybackSample`; independent advancement is a timing bug.
- Invariant 9: Retimed audio cache selection must prove coverage for the requested mix range before returning a segment.
- Invariant 10: Playback FaceDetections overlays must be selected from source-frame-indexed cache data for the current requested media source frame. If cache preparation lags, previous overlays may be held only within the explicit source-frame drift tolerance and must otherwise be cleared instead of shown stale.

## Verification Matrix
- Playhead clock policy and warp normalization:
  - Code: `editor_shared_media.cpp`, `playback_clock_coordinator.cpp`, `editor_playback.cpp`
  - Tests: `tests/test_playback_policy.cpp`
- Pitch-preserving time-stretch cache and playback gating:
  - Code: `audio_engine.h`, `editor_playback.cpp`
  - Tests: `tests/test_audio_time_stretch.cpp`, `tests/test_audio_time_stretch_cache.cpp`, `tests/test_playback_policy.cpp`
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
  - Code: `editor_shared_transcript.cpp`, `opengl_preview_window_transcript.cpp`, `direct_vulkan_preview_window.cpp`, `direct_vulkan_preview_geometry.cpp`, `render_decode.cpp`
  - Tests: `tests/test_transcript_logic.cpp` speaker-tracking cases
- Shared preview/render transcript overlay layout path:
  - Code: `editor_shared_transcript.cpp`, `opengl_preview_window_transcript.cpp`, `direct_vulkan_preview_window.cpp`, `direct_vulkan_preview_interaction.cpp`, `render_decode.cpp`
  - Tests: `tests/test_transcript_logic.cpp::testTranscriptOverlayLayoutHelperMatchesSectionLayout`
- Transform skip-aware timing progression:
  - Code: `transform_skip_aware_timing.cpp`
  - Tests: `tests/test_transform_skip_aware_timing.cpp`

## Failure Playbook
- Chief playbook issue:
  - Do not treat symptoms as independent clocks. A/V/caption drift is usually a violation of the single-clock contract, not a caption offset problem.
  - First establish the authoritative `absolutePlaybackSample`, the audio-master sample if enabled, and whether pitch-preserving audio is blocked.
  - Only then inspect derived domains such as media source frame, transcript frame, or retimed cache sample.
- Symptom: clicking a transcript word seeks correctly, but follow highlight drifts during playback.
  - Check: ensure follow path is consuming source frame from `transcriptFrameForClipAtTimelineSample(...)`.
  - Probe: `/profile?live=1&force=1` (`absolute_playback_sample`, playhead ages), then verify selected clip transcript path and marker set.
- Symptom: captions match video but not audible audio.
  - Check: this is a clock-source split. Compare `absolute_playback_sample`, `preview.current_sample`, and `audio.current_sample`.
  - Check: if non-1.0 `time_stretch` is active and `audio_playback_blocked` or `pitch_preserving_audio_blocked` is true, video/captions must be held until the retimed segment is ready.
  - Check: timer fallback during this condition is invalid.
- Symptom: preview transcript overlay timing differs from export render.
  - Check: both preview and render must use transcript-frame mapping from timeline sample/frame plus render-sync markers.
  - Probe: compare preview clip/time against a rendered frame at same timeline frame.
- Symptom: speech filter “skip all words” still passes clip audio.
  - Check: `transcriptWordExportRanges(...)` output should be empty for that clip coverage.
  - Probe: inspect effective ranges via `/state?live=1` and `/profile?live=1&force=1`, then verify active transcript file content.
- Symptom: transforms animate during silent/skipped sections under skip-aware mode.
  - Check: whether `setTransformSkipAwareTimelineRanges(...)` was refreshed after transcript edits.
  - Probe: verify speech filter/timing ranges snapshot and transform interpolation inputs.
- Symptom: transcript follow or overlay breaks only when render-sync markers exist.
  - Check: any bypass of marker-aware conversion helpers.
  - Probe: validate marker list and compare mapped source frame with/without markers for same timeline point.
- Symptom: video playback looks glitched or bounces between a few frames at non-1.0 `time_stretch` speed.
  - Check: audio profile for `audio_playback_blocked`, `pitch_preserving_audio_blocked`, `last_mix_silence_reason=input_out_of_range`, and out-of-range retimed segment bounds.
  - Probe: `/profile?live=1&force=1` audio fields `last_mix_out_of_range_timeline_sample`, `last_mix_out_of_range_source_sample`, `last_mix_out_of_range_normalized_sample`, `last_mix_out_of_range_audio_start_sample`, and `last_mix_out_of_range_audio_end_sample`.
  - Check: video pipeline may be healthy even if playback is visually stuck; confirm with `/pipeline?live=1` frame lag and handoff status.

## Operational Checklist
- Before merging timing changes:
  - Run `ctest --test-dir build --output-on-failure`.
  - Run transcript follow tests and playback policy tests specifically.
  - Validate one manual scenario with render-sync markers and speech filter enabled.
- Before release:
  - Capture `/profile?live=1&force=1` snapshots at idle and during playback.
  - Confirm preview/export transcript alignment at at least one known timestamp.
