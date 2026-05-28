# Detection Speed Improvement Plan

## Current Diagnosis

The current detector run does not appear to saturate the GPU. Samples showed moderate detector SM utilization with decode activity, CPU activity, and continuous artifact writes. That pattern usually means the detector is not being fed continuously enough, not necessarily that the GPU model is too small or that a second detector is immediately required.

Do not start by adding another detector worker. First make the existing pipeline measurable, then remove serialization between decode, detection, tracking, writing, and UI updates.

## Goals

- Keep the GPU detector fed with ready frames.
- Keep disk writes and CPU tracking off the detector hot path.
- Preserve deterministic output and resumable checkpoints.
- Keep UI controls responsive without updating them per frame.
- Make fallback, throttling, and performance limits explicit.

## Phase 1: Measure Stage Costs

Add per-stage timing to the detector runtime stats and summary output.

Track at minimum:

- Decode and frame acquisition time.
- Hardware handoff or upload time.
- Preprocess time.
- ncnn/SCRFD inference time.
- Postprocess/NMS time.
- Continuity tracking time.
- Artifact serialization/write time.
- UI/event pumping time.
- Total frame wall time.

Expose rolling averages in the detection control window and write aggregate values into `summary.json`.

Acceptance criteria:

- A single run identifies the dominant stage without external profiling.
- The controls window shows useful throughput numbers at a fixed refresh rate.
- `summary.json` contains enough timing data to compare two runs objectively.

## Phase 2: Throttle UI and Progress Updates

Update progress and controls on a fixed interval instead of every processed frame.

Recommended defaults:

- Controls window refresh: 4-10 Hz.
- Console progress: every 1-2 seconds or meaningful percentage change.
- Preview refresh: independent of detector frame processing.

Acceptance criteria:

- UI remains responsive.
- Detector throughput does not drop when the controls window is visible.
- No per-frame UI updates remain on the detection hot path.

## Phase 3: Move Artifact Writes Off the Hot Path

Introduce a bounded writer queue for detection records and checkpoint updates.

Best-practice constraints:

- Preserve resumability.
- Flush checkpoints at explicit intervals.
- Make queue backpressure visible in stats.
- Do not allow unbounded memory growth.

Acceptance criteria:

- Detector work is not blocked by normal artifact writes.
- If the writer falls behind, the control window reports writer backlog.
- Crash/restart behavior remains explicit and testable.

## Phase 4: Pipeline Decode, Detection, and Tracking

Split the serial loop into bounded stages:

- Decode/frame acquisition producer.
- GPU detection consumer.
- CPU postprocess/tracking stage.
- Artifact writer stage.

Use bounded queues so memory stays predictable and backpressure is explicit.

Recommended starting point:

- 2-4 decoded frames in flight.
- 1 detector worker.
- 1 tracking/postprocess worker if tracking is measurable as a bottleneck.
- 1 writer worker.

Acceptance criteria:

- GPU detection is fed from a ready-frame queue.
- Decode can run ahead when detector is busy.
- Writer and tracking stalls do not idle the detector unless queues are full.
- Output ordering remains deterministic.

## Phase 5: Multi-Inflight Detection

If the GPU is still underutilized after pipelining, allow multiple in-flight detector jobs.

Preferred approach:

- Reuse one detector context if ncnn/Vulkan supports safe multi-inflight execution.
- Otherwise create a small pool with explicit memory accounting.
- Start with two in-flight jobs, benchmark before increasing.

Acceptance criteria:

- Throughput improves against the single-detector pipelined baseline.
- GPU memory remains bounded.
- Results remain deterministic or ordering is restored before writing artifacts.

## Phase 6: Evaluate Two Detector Workers

Two detector workers should be treated as an experiment, not the default design.

Risks:

- GPU context contention.
- Higher VRAM use.
- Worse cache behavior.
- More complex synchronization and checkpointing.
- Lower throughput if decode or tracking is the real bottleneck.

Only enable behind a runtime setting after the earlier phases are measured.

Acceptance criteria:

- Two workers beat one worker on the same clip and settings.
- The improvement is repeatable.
- GPU utilization and wall-clock throughput both improve.
- Memory and checkpoint behavior remain safe.

## Benchmark Protocol

Use the same clip, detector settings, start frame, frame count, and output mode for each run.

Record:

- Frames per second.
- Processed frames per second.
- Total wall time.
- Average inference time.
- Average decode time.
- Average write time.
- GPU SM utilization.
- GPU decode utilization.
- CPU utilization.
- Peak RSS.
- Peak GPU memory.

Compare against the current serial baseline before accepting any architecture change.

## Implementation Order

1. Add timing instrumentation and summary fields.
2. Throttle UI/progress updates.
3. Add bounded async artifact writer.
4. Add bounded decode-to-detect pipeline.
5. Add optional multi-inflight detection.
6. Benchmark two detector workers behind an explicit flag.

This keeps the implementation simple at each step and avoids adding concurrency before the actual bottleneck is known.

## Implemented

- Runtime controls now show average decode, handoff/prep, inference wall, tracking, and checkpoint-enqueue times.
- `summary.json` now records the same stage averages and sample counts.
- Control-window updates are throttled to avoid per-frame UI refresh on the detector hot path.
- Live preview is decoupled from the detector critical path by default; socket/file preview can still require synchronized output.
- Live preview uses a bounded latest-sample buffer so stale preview frames are dropped instead of blocking detection.
- The existing decoder-direct pipeline now uses a configurable bounded in-flight slot count, defaulting to four.
- `summary.json` records `decoder_direct_pipeline_slots` so benchmark runs identify the active pipeline depth.
- `summary.json` records whether live preview was enabled and whether preview synchronization was required.
- `summary.json` records live preview queue capacity, presented samples, and dropped samples.
- Streaming checkpoint writes now use a bounded asynchronous writer by default.
- The controls window reports checkpoint writer backlog.
- `summary.json` records checkpoint writer queue capacity, max backlog, backpressure, queued records, written records, and average writer-thread write time.
- Detector worker count is an explicit benchmark setting. Multi-worker decoder-direct runs use independent detector worker objects and asynchronous prepared GPU inference, with results consumed in source-frame order.
- `--benchmark-pipeline-slots [CSV]` runs isolated child benchmarks for pipeline depths, defaults to `1,4,8`, and writes `pipeline_benchmark_summary.json`.
- Pipeline benchmark child runs force preview/control/progress off and use fresh per-slot output directories so checkpoint resume files cannot contaminate comparisons.
- The benchmark summary records processed FPS, wall time, stage timing, writer timing/backpressure, zero-copy status, and detector-worker fallback status for each run.

## Explicit Completion Boundary

- Concurrent detector workers are enabled only for the decoder-direct prepared-frame path, where each worker owns its detector instance and no shared detector object is called concurrently.
- This completes the improvement plan through measurable pipeline depth benchmarking, bounded queues, explicit fallback reporting, deterministic ordered output, and independent multi-worker detector execution where the current architecture can support it.
