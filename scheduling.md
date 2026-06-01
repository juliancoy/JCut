# Scheduling

## Purpose

This document defines how JCut runtime scheduling should work for playback, visible decode, prefetch, audio readiness, and overlay preparation. It is the companion to `TIME.md`: `TIME.md` defines temporal domains and conversion truth; this file defines which work is scheduled, prioritized, blocked, or dropped.

## Core Principle

Playback has one temporal source of truth: the active timeline sample in 48 kHz timeline-sample space.

Every scheduler must derive its target work from that sample through the canonical conversion helpers. No scheduler may compare or prioritize work across mixed domains without converting first.

Examples:

- Audio clock reports timeline samples.
- UI timeline position is timeline frames.
- Decoder requests are media source frames.
- Transcript and captions use transcript/source-derived frames.
- Visible decode priority must compare timeline distance, not source-frame distance.

## Runtime Ownership

- `EditorWindow` owns the playback playhead and clock policy.
- `AudioEngine` owns audible output state, audio buffering, underrun state, and pitch-preserving sidecar readiness.
- `PreviewSurface` owns preview-visible frame requests and presentation state.
- `TimelineCache` owns decode request coalescing, visible-frame priority, prefetch, pending-request state, and frame residency.
- `AsyncDecoder` owns decode worker queues and decode execution.
- Overlay workers own expensive frame-local overlay preparation during playback.
- The Vulkan presenter owns GPU presentation; it must not become a decode scheduler or temporal clock.

## Playback Start Gate

Playback should not start until required media for the selected clock mode is ready.

Required gates:

- If audio is the master clock, audio stream state must be open/running and able to report an effective audible sample.
- If pitch-preserving `time_stretch` is active, the needed retimed audio sidecar/cache must be ready for the requested start frame.
- Visible video should have a small current/future readiness window, but video should not silently take over the clock.
- Missing non-critical overlays must not block playback.

If a gate fails, the UI should report the blocked reason explicitly:

- `waiting_for_retimed_audio`
- `generating_retimed_audio`
- `waiting_for_visible_frames`
- `audio_stream_not_ready`
- `decode_queue_starved`

## Audio Scheduling

Audio is scheduled from timeline samples.

Rules:

- Audio-master playback follows `AudioEngine::currentSample()`, which should represent effective audible output position, not queued future position.
- Pitch-preserving non-1.0 playback uses precomputed retimed audio only.
- There is no implicit SOLA or varispeed fallback when `time_stretch` sidecar-only mode is required.
- If required retimed audio is missing, playback holds before start or enters an explicit blocked state.
- Audio readiness and progress must be observable through REST/perf diagnostics.

Audio must not drive video forward while reporting a stale, blocked, or rewound current sample.

## Visible Decode Scheduling

Visible video decode requests are scheduled in media source-frame space, but prioritized in timeline-frame space.

Required conversion:

```text
timeline_sample -> timeline_frame -> media_source_frame
media_source_frame -> approximate timeline_frame for priority distance
```

Rules:

- A visible frame at the current playhead has highest priority.
- Near-future visible frames should have high priority.
- Far prefetch work must never starve current visible work.
- Pending visible requests should be coalesced by clip id and media source frame.
- A small bounded visible backlog is valid; a backlog of one is only safe if decode latency is always below frame cadence.
- Completed visible frames may be late, but a late hardware frame should still be cached if it can help nearest-frame presentation.
- Strict Vulkan preview requests must reject CPU-image payloads in the direct Vulkan path.

The most important invariant:

Do not compare media source-frame numbers directly against timeline playhead frames.

On a 60 fps source inside a 30 fps timeline, source frame `522000` may correspond to timeline frame `261000`. Treating those as one domain makes current visible frames look far away and assigns them low priority.

## Prefetch Scheduling

Prefetch is opportunistic. It improves smoothness but is never correctness-critical.

Rules:

- Prefetch derives future timeline frames from the active playhead, direction, playback speed, speech ranges, and lookahead settings.
- Prefetch converts each future timeline frame to media source frame before request.
- Prefetch must respect decode queue pressure and visible pending pressure.
- Prefetch requests use lower priority than visible requests.
- Prefetch may be dropped, superseded, or skipped without affecting playback correctness.
- Prefetch should not trigger implicit CPU decode/upload fallback for direct Vulkan preview.

## Decode Queue Policy

Decode workers should prefer current visible work over stale or speculative work.

Rules:

- Visible requests can supersede older queued work for the same file when moving forward.
- Supersession must be bounded and diagnosable; large `superseded` counts during playback indicate a scheduling bug or insufficient lookahead.
- Queue-full behavior must return explicit diagnostics.
- Request callbacks returning null must carry a reason in aggregate counters.
- Queue priority must be calculated after mapping source-frame requests back into the timeline domain.

Expected diagnostic signals:

- `visible_request_attempt_rate`
- `visible_request_dispatch_rate`
- `visible_request_blocked_fraction`
- `pending_visible_requests`
- `visible_decode.hardware_completed`
- `visible_decode.null_completed`
- `decoder_diagnostics.null_callbacks.superseded`
- `decode_timing.last_total_latency_ms`
- `playback_smoothness.exact_hit_rate`
- `playback_smoothness.avg_frame_lag`

## Backpressure

Backpressure should reduce speculative work first.

Order of shedding:

1. Drop stale prefetch.
2. Reduce prefetch per tick.
3. Coalesce duplicate visible requests.
4. Increase visible lookahead/backlog within configured bounds.
5. Report visible starvation.

Backpressure must not:

- Switch render backends implicitly.
- Fall back to CPU image upload in direct Vulkan preview.
- Advance an independent video clock.
- Hide a sidecar/audio blocked state.

## Overlay Scheduling

Overlays are scheduled from the same active playhead sample as video and captions.

Rules:

- During playback, expensive FaceDetections/continuity overlay preparation runs on a worker.
- The UI thread applies the latest valid typed snapshot.
- Stale overlay worker results are dropped by request key/generation.
- Missing or late overlays should keep the prior snapshot briefly rather than block playback.
- Hit testing uses the currently applied snapshot, regardless of selected tab.
- Captions and speaker labels use the shared transcript/source timing path and the Vulkan text renderer.

Overlay work must not parse artifacts or rebuild continuity streams on the playback hot path.

## Presentation Scheduling

The direct Vulkan presenter presents the best available frame for the current playhead.

Rules:

- Exact current frame is preferred.
- Near-current cached hardware frame is acceptable when exact decode is late.
- Presentation diagnostics must report exact/approx/missing state and frame lag.
- The presenter must not request OpenGL fallback.
- The presenter must not create CPU upload fallback for normal direct Vulkan preview.
- Text and overlays should be prepared as typed draw inputs and rendered through Vulkan passes.

## Failure Modes And Meaning

- High `exact_hit_rate` with low FPS: presentation or GPU path problem.
- Low `exact_hit_rate`, high `avg_frame_lag`, many visible null callbacks: decode scheduling problem.
- High `superseded` count: requests are being chased faster than they can complete or priority/lookahead is wrong.
- `visible_request_already_pending` with frame lag increasing: visible backlog/lookahead is too small or current request is stuck.
- Audio underruns with video smooth: audio buffering or sidecar coverage problem.
- Captions drift with audio/video stable: temporal-domain conversion problem.
- Overlay click/hover unavailable while boxes visible: applied overlay snapshot/hit-test state problem, not decode.

## Implementation Invariants

- Invariant 1: Runtime playhead scheduling starts from timeline sample truth.
- Invariant 2: Decode request targets are media source frames.
- Invariant 3: Decode priority distance is timeline-frame distance.
- Invariant 4: Visible decode outranks prefetch.
- Invariant 5: Pitch-preserving sidecar-only audio has no implicit runtime fallback.
- Invariant 6: Direct Vulkan preview has no implicit OpenGL or CPU-image upload fallback.
- Invariant 7: Overlay workers do not own clocks and do not block playback.
- Invariant 8: REST/perf diagnostics must expose enough state to distinguish audio gate, decode starvation, overlay lag, and presentation failure.

## Test Requirements

Regression tests should cover:

- Timeline/source frame mapping at 30 fps timeline and 60 fps source.
- Decode priority calculation for current visible frames at 1.0x and 1.5x.
- Visible requests requiring hardware/GPU payloads in direct Vulkan preview.
- Pitch-preserving sidecar-only audio start gating.
- Caption timing derived from source/transcript conversion at multiple playback speeds.
- Overlay snapshot worker coalescing and stale result dropping.
- REST/perf fields for audio readiness, visible decode state, pending visible count, and smoothness.

## Practical Debugging Playbook

When playback is choppy:

1. Check `/audio`; confirm stream running, no current underrun, and sidecar readiness.
2. Check `/pipeline.preview.playback_smoothness`; compare exact hit rate, frame lag, and presented FPS.
3. Check `/pipeline.preview.visible_decode_diagnostics`; confirm hardware completions versus null completions.
4. Check decoder null callback reasons; high `superseded` means scheduling is chasing.
5. Check pending visible requests and block reason.
6. Check handoff/presentation metrics only after decode is delivering usable hardware frames.
7. Check overlay worker metrics separately; overlay lag should not explain black or stale video unless it blocks UI/presentation.

## Current Long-Term Shape

The intended long-term shape is simple:

- One clock.
- Canonical conversions.
- Visible decode prioritized by timeline distance.
- Prefetch as disposable speculative work.
- Sidecar-only audio readiness as an explicit gate.
- Direct Vulkan presentation with no implicit fallback.
- Worker-prepared overlays that never own time.
- Diagnostics that name domains and blocked states explicitly.
