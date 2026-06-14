# TIME.md

> **Doc status (2026-06-13):** This document defines JCut's intended timing model.
> If code disagrees with this document, either fix the code or update this file in the
> same change. Do not leave temporal behavior implicit.

## Purpose

JCut must keep audio, video, subtitles, transcript follow, speaker labels, overlays,
and transforms in sync by deriving them from one runtime clock: the system monotonic
transport clock.

The runtime clock is not audio. It is not video. It is not an audio sample counter.
It is elapsed monotonic system time plus the active transport state: start position,
playback speed, pause state, and effective playback ranges.

## Runtime Rule

During playback, every visible or audible consumer must answer the same question:

```text
What timeline time does the system transport clock say it is right now?
```

From that timeline time, consumers convert into their own domains:

- audio output sample
- video source frame
- subtitle/transcript frame
- FaceDetections source frame
- speaker label frame
- transform interpolation time
- filtered speech-range time

No subsystem may advance its own independent playback clock. Audio device callbacks,
decoder PTS, cached video frame numbers, and subtitle row indices are feedback or
lookup data only.

## Core Temporal Domains

- `transportTimeSeconds`: elapsed timeline seconds produced from the system monotonic
  clock, playback speed, pause/seek anchors, and effective playback ranges.
- Timeline/edit frame domain (`kTimelineFps = 30`): UI timeline, clip placement,
  edit keyframes, export range bookkeeping, and fixed-rate render loops.
- Clip-local timeline time/frame domain: position inside a clip after subtracting
  clip start.
- Source media time/frame/sample domain: decoded media position after clip trim,
  clip playback rate, source FPS, source sample rate, and render-sync markers.
- Media transfer/decode frame domain: the actual source frame requested from the
  decode pipeline. This is never a 30 fps clock unless source FPS is genuinely
  unknown and the fallback is explicitly logged.
- Transcript frame domain (`30 fps` transcript clock): transcript words/sections,
  subtitles, follow/highlight, and speaker lookup.
- Filtered timeline domain: sparse timeline spans produced from speech ranges.

## System Clock Transport

Playback state is anchored by:

- `anchorSystemTime`: monotonic system timestamp when playback started or resumed.
- `anchorTimelineTime`: timeline time at that moment.
- `playbackSpeed`: user transport speed.
- `effectivePlaybackRanges`: continuous or speech-filtered ranges.
- `timelineAdvanceCarrySamples`: fractional-sample accumulator that absorbs timer
  jitter across ticks (see below).

### Delta-Accumulation Model

The runtime clock is driven by a `QTimer` (or equivalent) that calls
`EditorWindow::advanceFrame()` on each tick. Unlike a pure monotonic formula,
the implementation uses **delta accumulation** with a carry-over fractional-sample
buffer:

```text
elapsedMs = nowMs - lastTickMs
lastTickMs = nowMs

carrySamples += elapsedSeconds * audioSampleRate * speed
deltaSamples = floor(carrySamples)
carrySamples -= deltaSamples
```

This is equivalent to the ideal formula over long windows but tolerates timer
jitter, main-thread contention, and variable-rate timer intervals without losing
or gaining fractional samples. The carry accumulator ensures that sub-sample
time is preserved across ticks rather than truncated.

### Timer Jitter Tolerance

Because `advanceFrame()` runs on the Qt main-thread timer, its callback may be
delayed by:

- Decoder completion handlers
- UI layout and paint events
- OS scheduler preemption

The delta-accumulation model handles this correctly: a delayed tick produces a
proportionally larger `deltaSamples`, catching up to the correct timeline
position. An early tick produces a smaller step, with the remainder held in
`carrySamples` for the next tick. No time is gained or lost.

### Fallback Minimum Step

If `deltaSamples <= 0` (e.g., the timer fired faster than one sample's worth of
real time), a minimum step of one frame at the current speed is forced. This
prevents playback from stalling on very fast timer intervals.

### Anchor Reset Events

Seek, pause, speed changes, warp-mode changes, and range changes reset the
transport anchor:

- `lastTickMs` is set to the current monotonic time.
- `carrySamples` is cleared to zero.
- `transportTimelineSample` is set directly to the target position.

These events do not create a new clock. They only re-anchor the existing system
monotonic transport.

### Speech-Filtered Projection

When speech filtering is enabled, elapsed transport time is projected through the
filtered playback ranges. The system clock still drives playback; the range
mapping only changes which timeline spans are visited. The delta-accumulation
model applies unchanged — only the `effectivePlaybackRanges` set differs.

## Conversion Paths

- System clock -> timeline time:
  - `EditorWindow::advanceFrame()` advances `m_transportTimelineSample` by a
    delta computed from elapsed monotonic time, playback speed, and the
    carry-over fractional-sample accumulator. See "Delta-Accumulation Model"
    above.
- Timeline time -> timeline frame:
  - `floor(timelineSeconds * kTimelineFps)` or the shared helper used by the editor.
- Timeline time/frame -> source media frame/sample:
  - `sourceFrameForClipAtTimelinePosition(...)`
  - `sourceSampleForClipAtTimelineSample(...)`
  - These must apply clip trim, playback rate, source FPS/sample rate, and render-sync
    markers.
- Timeline/source time -> transcript frame:
  - Convert source media seconds to the 30 fps transcript clock.
  - `transcriptFrameForClipAtTimelineSample(...)`
  - `transcriptFrameForClipSourceFrame(...)`
- Facestream stored frame -> source media frame:
  - `mapFacestreamFrameToSourceFrame(...)`
  - Output is a source/decode frame, not a timeline frame.

## Audio

Audio follows the system transport clock.

The audio engine should render the chunk that corresponds to the current transport
timeline time. Its device position and queued buffer depth are diagnostics used to
measure latency, underruns, and drift.

Important rules:

- Do not use the audio callback position as the master playback clock.
- Do not use queued audio end position as "now"; it is future buffered audio.
- Output latency should be reported separately from transport time.
- Bounded drift correction may retime or refill audio, but it must not redefine the
  transport clock.
- If pitch-preserving time-stretch data is unavailable, audio readiness may block or
  degrade audio according to policy; video and subtitles must not switch to an audio
  clock to hide that problem.

### Drift Correction Model

Audio hardware and the system transport clock can drift apart due to:
- Audio device clock rate inaccuracy (typical crystal tolerance: ±50 ppm).
- Resampling artifacts in the audio pipeline.
- Cumulative scheduling latency between timer ticks.

Drift correction is implemented as a closed-loop feedback controller in
`evaluatePlaybackDriftRetimeMultiplier(...)`. It computes a multiplier applied
to the audio output rate to keep it aligned with the transport clock.

Key parameters (see `PlaybackDriftRetimeInput`):

- `deadbandSamples` (default 3840, ~80 ms at 48 kHz): Drift within this window
  is ignored. This prevents unnecessary correction from benign timer jitter.
- `fullCorrectionSamples` (default 60000, ~1250 ms): Drift at or beyond this
  magnitude applies the full `maxCorrection` rate adjustment.
- `maxCorrection` (default 0.01, i.e. ±1%): Hard limit on the retime multiplier
  deviation from 1.0. This prevents audible pitch warble from aggressive correction.
- `smoothing` (default 0.035): First-order IIR smoothing coefficient. The
  multiplier moves toward the target exponentially, avoiding abrupt rate changes.

The correction is applied as a rate multiplier to the audio output, not as a
clock override. The transport clock remains authoritative; the audio engine
adjusts its output rate to track it. This is consistent with Invariant 3.

## Video

Video follows the system transport clock.

For each presentation tick:

1. Compute current timeline time from the system transport clock.
2. Find the active clip at that timeline time.
3. Convert to source media frame with marker-aware mapping.
4. Present the exact frame if available.
5. If an approximate frame is used, it must be within the configured stale-frame
   tolerance. Anything older must be dropped or shown as visibly stale in diagnostics.

Decoder PTS and cache completion order are readiness signals only. They must not
advance or delay the runtime clock.

## Subtitles And Transcript Follow

Subtitles and transcript follow must use the same timeline/source mapping as video.

For live preview, when a video frame has already been selected for presentation,
subtitle overlay timing should prefer the presented source frame. That keeps text
attached to the image if video presentation is late. This is still derived from the
system transport path because the presented frame was selected from the transport
time request.

Do not match subtitles by table row order, render order, or independent timers.

## FaceDetections And Speaker Labels

FaceDetections artifacts are source-media artifacts.

Supported domains:

- `FacestreamFrameDomain::SourceRelative`
  - Stored frame is relative to `clip.sourceInFrame`.
  - Source mapping: `clip.sourceInFrame + keyframe.frame`.
- `FacestreamFrameDomain::SourceAbsolute`
  - Stored frame is an absolute source/decode frame.
  - Source mapping: `sourceFrame = keyframe.frame`.

Historical `ClipTimeline30Fps` data is legacy/manual-only and must not be used for
generated FaceDetections artifacts.

Speaker and subtitle lookup uses transcript-frame time:

```text
mediaSourceFrame -> sourceSeconds -> transcriptFrame at 30 fps
```

Never compare media source frames directly to transcript frames. On a 60 fps source,
source frame `258214` means about `4303.57s`, which maps to transcript frame
`129107`; treating `258214` as a transcript frame means about `8607.13s`.

## Speech-Filtered Playback

Speech filtering changes timeline traversal, not the clock source.

- `effectivePlaybackRanges()` defines the timeline spans to visit.
- The system transport clock advances elapsed playback time.
- Filtered projection maps elapsed playback time into the kept timeline spans.
- Audio, video, subtitles, transforms, and overlays consume the projected timeline
  time.

Skipping silent spans must not create separate audio/video/subtitle clocks.

## Render And Export

Offline render/export is deterministic fixed-step playback, not live system-clock
playback.

Each output frame has an explicit output time. That output time is converted through
the same timeline/source/transcript helpers used by preview. Export must not use
wall-clock pacing, audio callback progress, or decoder completion order as timing
authority.

## Diagnostics

Any diagnostic field or log that crosses domains must name the domain explicitly:

- `transport_time_seconds`
- `timeline_frame`
- `timeline_seconds`
- `media_source_frame`
- `media_source_seconds`
- `transcript_frame`
- `audio_output_sample`
- `audio_latency_samples`
- `retimed_cache_sample`

Avoid ambiguous names like `current_frame` or `source_frame` unless the surrounding
object makes the domain unambiguous.

Useful sync diagnostics:

- requested timeline time from the system transport clock
- requested media source frame
- presented media source frame
- subtitle transcript frame
- audio output latency
- audio underrun/starvation state
- video stale-frame delta
- whether speech-filter projection is active

## Why Desync Happens

Audio, video, and subtitles go out of sync when any subsystem answers "what time is
it?" differently.

Common failure modes:

- Audio reports future queued position as current audible time.
- Video presents a stale cached frame as if it matched the current transport time.
- Subtitles use the timeline playhead while video is actually showing an older frame.
- Transcript logic compares 60 fps media source frames directly to 30 fps transcript
  frames.
- Render-sync marker adjustments are applied in one path but bypassed in another.
- Speech-filtered playback advances one consumer through filtered time and another
  through unfiltered timeline time.
- Pitch-preserving audio cache misses make audio stall while video/subtitles continue
  without clear readiness diagnostics.

The fix is always the same: derive the requested time from the system transport
clock once, convert through the shared helper for each domain, and log the exact
domain at each boundary.

## Invariants

- Invariant 1: Live playback uses monotonic system time as the only runtime clock.
- Invariant 2: Audio, video, subtitles, transcript follow, speaker labels, overlays,
  and transforms derive from the same transport time.
- Invariant 3: Audio device position is latency/drift feedback, not transport time.
- Invariant 4: Decoder PTS and video cache order are readiness feedback, not master
  time.
- Invariant 5: Subtitle timing follows source/transcript mapping; in live preview it
  may use the presented source frame to stay visually attached to late video.
- Invariant 6: Source media frames and transcript frames are different domains and
  must be converted through seconds/source FPS.
- Invariant 7: Render-sync markers are part of timeline-to-source conversion for
  every runtime consumer.
- Invariant 8: Speech-filter playback changes range projection only; it does not
  create separate clocks.
- Invariant 9: Retimed audio cache selection must prove coverage before use.
- Invariant 10: Stale video or overlay reuse is a bounded display hold, not evidence
  that playback time stopped.
