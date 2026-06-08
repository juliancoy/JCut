# Vulkan FaceDetections Offscreen Modularization Plan

## Goal

Reduce `vulkan_facedetections_offscreen_main.cpp` from a 7k-line monolith into focused modules while preserving:

- `runVulkanFacestreamOffscreen(const QStringList& args)` as the embedded runner API.
- `JCUT_FACESTREAM_OFFSCREEN_STANDALONE` `main()` behavior.
- Existing output artifacts, checkpoint/resume compatibility, summary fields, and CLI flags.
- Zero-copy decoder-direct detection behavior and live preview behavior.

The final `vulkan_facedetections_offscreen_main.cpp` should only contain standalone `main()` glue, or be removed in favor of a small runner translation unit.

## Proposed Modules

### `vulkan_facedetections_offscreen_options.h/.cpp`

Owns command-line and user-facing run configuration.

- `struct VulkanFaceDetectionsOffscreenOptions`
- `parseVulkanFaceDetectionsOffscreenArgs(int argc, char** argv, ...)`
- `printVulkanFaceDetectionsOffscreenUsage(const char* argv0)`
- `backendIdForOptions(...)`
- `loadClipFromJsonPath(...)`
- `detectorSettingsPathForVideo(...)` wrapper if this runner needs a local alias
- benchmark argument filtering helpers currently near `benchmarkBaseArgs(...)`

Notes:

- Keep this free of Vulkan, detector, preview-window, and artifact writer dependencies.
- Prefer mapping into existing `jcut::facedetections::DetectorRuntimeSettings` rather than duplicating fields long term.

### `vulkan_facedetections_offscreen_tuning.h/.cpp`

Owns runtime tuning and persisted parameter JSON.

- `struct RuntimeTuning`
- `struct PreviewDebugSettings`
- `trackingTuningForRuntime(...)`
- `runtimeTuningToJson(...)`
- `previewDebugSettingsToJson(...)`
- `applyPreviewDebugSettingsObject(...)`
- `runtimeTuningFromDetectorSettings(...)`
- `applyRuntimeParamsFile(...)`
- `saveRuntimeTuningFile(...)`

Notes:

- This module should become the only bridge between detector settings files and runner-local tuning.
- Keep Qt JSON dependencies here, not in detector execution modules.

### `vulkan_facedetections_offscreen_detection_filters.h/.cpp`

Owns raw detection cleanup and frame-domain transformations.

- `struct DetectionSanitizeStats`
- `sanitizeDetections(...)`
- `logDetectionSanitizeStats(...)`
- `flipDetectionsVertically(...)`
- `buildRawDetectionFrameRecord(...)`

Notes:

- This module depends on `facedetections_types`/tracking JSON helpers but not Vulkan.
- It should be easy to unit-test with synthetic boxes.

### `vulkan_facedetections_offscreen_progress.h/.cpp`

Owns terminal progress and ETA calculations.

- `struct AdaptiveEtaSample`
- `struct AdaptiveEtaEstimate`
- `class AdaptiveEtaTracker`
- `formatDuration(...)`
- `renderProgressLine(...)`
- `shouldRenderProgress(...)`

Notes:

- Keep this CLI-only. No Qt widgets, no detector state.

### `vulkan_facedetections_offscreen_control_panel.h/.cpp`

Owns the optional Qt control window.

- `struct DetectorControlPanel`
- `struct DetectorStageTimingTotals`
- `struct DetectorLiveTelemetrySnapshot`
- `livePipelineStateText(...)`
- `liveNcnnBreakdownText(...)`
- `percentText(...)`, `areaRatioText(...)`, `aspectText(...)`, `millisecondsText(...)`
- `syncDetectorControlPanel(...)`
- `syncDetectorPreviewPanel(...)`
- `updateDetectorRuntimeStats(...)`
- `createDetectorControlPanel(...)`

Notes:

- This module can depend on Qt Widgets and `imgui_preview_window.h`.
- The runner loop should pass snapshots and callbacks into this module instead of letting UI code reach into processing internals.

### `vulkan_facedetections_offscreen_preview_io.h/.cpp`

Owns preview frame generation and external preview socket output.

- `struct LivePreviewSample`
- `buildPreview(...)`
- `sendPreviewFrame(...)`
- any queue/sample helpers for live preview presentation that are currently local lambdas in the main loop

Notes:

- Keep image drawing and socket transport separate from detector inference.
- ImGui window presentation can stay in the control-panel module if the coupling is mostly UI state, or move here if presentation queue ownership is clearer.

### `vulkan_facedetections_offscreen_vulkan_harness.h/.cpp`

Owns standalone Vulkan context and reusable GPU buffers used by detector execution.

- `findMemoryType(...)`
- `class VulkanHarnessContext`
- buffer/image allocation helpers currently embedded in `VulkanHarnessContext`
- `findRes10NcnnModelFile(...)`
- `scrfdModelFileName(...)`

Notes:

- This module should be the only new module that directly owns raw Vulkan setup/teardown.
- Keep detector model discovery here only if it remains tightly coupled to detector initialization; otherwise move model discovery to `..._options` or a dedicated model module.

### `vulkan_facedetections_offscreen_detector_execution.h/.cpp`

Owns detector invocation paths.

- `readDetections(...)`
- `detectVulkanFrame(...)`
- `detectRes10VulkanFrame(...)`
- `detectRes10FromDecoderFrame(...)`
- `detectScrfdVulkanFrame(...)`
- `detectScrfdFromDecoderFrame(...)`
- OpenCV fallback functions guarded by `JCUT_HAVE_OPENCV`:
  - `hasVulkanDnnTarget(...)`
  - `initializeTrainedVulkanDnn(...)`
  - `detectTrainedVulkanDnn(...)`

Notes:

- This module depends on `vulkan_facedetections_offscreen_vulkan_harness`.
- It should return raw detections plus timing/error information, not mutate runner summary state.

### `vulkan_facedetections_offscreen_decoder_pipeline.h/.cpp`

Owns decoder-direct handoff preparation/finalization and worker slot state.

- `struct PreparedDecoderDetectionResult`
- `struct PreparedDecoderDetectionSlot`
- `struct DecoderDetectorWorker`
- `kMaxDecoderDirectPipelineSlots`
- `scrfdTensorBytesForSourceSize(...)`
- `prepareRes10DecoderFrame(...)`
- `finalizePreparedRes10DecoderFrame(...)`
- `prepareScrfdDecoderFrame(...)`
- `finalizePreparedScrfdDecoderFrame(...)`
- helper functions for starting/collecting worker futures now embedded in the main loop

Notes:

- This is the highest-risk module because it touches concurrency, decoder frame lifetime, Vulkan handoff, and detector instances.
- Extract after the pure helper modules are already split and covered by the existing offscreen tests.

### `vulkan_facedetections_offscreen_artifact_io.h/.cpp`

Owns artifact and checkpoint output primitives.

- `writeJson(...)`
- `writeBinaryJsonObject(...)`
- typed FaceDetections checkpoint record append/read helpers
- final summary writing helper
- raw detections/tracks/continuity artifact assembly helpers that currently live at the end of the runner

Notes:

- Prefer reusing `json_io_utils` and `facedetections_continuity_artifacts` directly.
- Keep schema strings centralized here so compatibility is easier to audit.

### `vulkan_facedetections_offscreen_checkpoint.h/.cpp`

Owns resumable checkpoint state and async checkpoint writing.

- `class AsyncFaceStreamWriter`
- `struct FaceDetectionsResumeState`
- `faceDetectionsResumeIndexPath(...)`
- `continuityTrackStateToResumeString(...)`
- `continuityTrackStateFromResumeString(...)`
- `completedFrameRangesToJson(...)`
- `completedFrameRangesFromJson(...)`
- `continuityTrackToResumeJson(...)`
- `continuityTracksFromResumeJson(...)`
- `saveFaceDetectionsResumeIndex(...)`
- `loadFaceDetectionsResumeIndex(...)`
- `loadFaceDetectionsResume(...)`

Notes:

- This module should have focused tests because it owns resumability and backward compatibility.
- It should not depend on Vulkan or Qt Widgets.

### `vulkan_facedetections_offscreen_benchmark.h/.cpp`

Owns pipeline-slot benchmark mode.

- `benchmarkBaseArgs(...)`
- `benchmarkSummaryRow(...)`
- `runPipelineSlotBenchmark(...)`

Notes:

- This should call the public runner API rather than duplicate setup.
- Keep benchmark output schema stable.

### `vulkan_facedetections_offscreen_runner.cpp`

Owns the orchestration loop after helpers have moved.

- `runVulkanFacestreamOffscreen(const QStringList& args)`
- internal `runVulkanFacestreamOffscreenWithArgv(...)`
- top-level lifecycle:
  - QApplication ownership
  - parse/preflight
  - decoder setup
  - detector/model setup
  - resume/checkpoint setup
  - processing loop
  - summary/artifact finalization

Notes:

- This file should become the only large file left, but the target should be under ~1200-1800 lines after extraction.
- Keep local lambdas only for orchestration-specific glue; move reusable logic into modules above.

### `vulkan_facedetections_offscreen_main.cpp`

Final standalone entry point only.

- Include `vulkan_facedetections_offscreen_runner.h`
- Provide `main()` under `JCUT_FACESTREAM_OFFSCREEN_STANDALONE`

Target size: under 40 lines.

## Dependency Direction

Preferred dependency flow:

1. Options/tuning/progress/detection filters/checkpoint/artifact IO are leaf-level utilities.
2. Vulkan context is the low-level GPU utility.
3. Detector execution depends on Vulkan context and tuning/filter types.
4. Decoder pipeline depends on detector execution, Vulkan context, frame handoff, and decoder frame handles.
5. Control panel and preview IO depend on tuning/progress snapshots but not detector internals.
6. Runner depends on all modules and performs orchestration.
7. Main depends only on runner.

Avoid reverse dependencies from helper modules back into the runner.

## Extraction Order

### Phase 1: Low-risk pure helpers

1. Extract options and usage into `vulkan_facedetections_offscreen_options`.
2. Extract runtime tuning and preview debug settings into `vulkan_facedetections_offscreen_tuning`.
3. Extract detection filtering/sanitization into `vulkan_facedetections_offscreen_detection_filters`.
4. Extract progress/ETA helpers into `vulkan_facedetections_offscreen_progress`.

Verification:

- Full build.
- Existing face detection tracking/artifact tests.
- Run `jcut_vulkan_facedetections_offscreen --preflight` compile path if GUI is available.

### Phase 2: IO and UI boundaries

1. Extract JSON artifact helpers into `vulkan_facedetections_offscreen_artifact_io`.
2. Extract checkpoint/resume and `AsyncFaceStreamWriter` into `vulkan_facedetections_offscreen_checkpoint`.
3. Extract preview socket/image helpers into `vulkan_facedetections_offscreen_preview_io`.
4. Extract Qt control panel into `vulkan_facedetections_offscreen_control_panel`.

Verification:

- Resume from an existing `.part` file.
- Confirm compact resume index is still accepted.
- Confirm summary JSON fields are unchanged.
- Run live preview/control-window smoke path on a short clip when GUI is available.

### Phase 3: GPU/detector execution

1. Extract `VulkanHarnessContext` and Vulkan memory helpers into `vulkan_facedetections_offscreen_vulkan_harness`.
2. Extract direct detector functions into `vulkan_facedetections_offscreen_detector_execution`.
3. Extract prepared decoder pipeline structs and functions into `vulkan_facedetections_offscreen_decoder_pipeline`.

Verification:

- Build with `JCUT_HAS_CUDA_DRIVER=1` and `0` if possible.
- Short run using decoder-direct SCRFD path.
- Short run with `--require-zero-copy`.
- Existing Vulkan subtitle/render tests should still compile because source lists will need the new modules.

### Phase 4: Runner shrink

1. Move orchestration from `vulkan_facedetections_offscreen_main.cpp` to `vulkan_facedetections_offscreen_runner.cpp`.
2. Reduce `vulkan_facedetections_offscreen_main.cpp` to standalone `main()` only.
3. Update CMake explicit source lists for `jcut_vulkan_facedetections_offscreen` and any tests/tools that compile this runner.

Verification:

- Full build.
- `jcut_vulkan_facedetections_offscreen --help`.
- Short frame-limited run on a known sample.
- Resume run against the same sample.
- Benchmark mode with a tiny max-frame count.

## Build System Updates

Add the new `.cpp` files to:

- `jcut_vulkan_facedetections_offscreen` explicit sources in `CMakeLists.txt`.
- Any tests or harnesses that directly compile the offscreen runner.

The main editor `editor_core` target uses globbing, but explicit standalone targets do not. Do not rely on globbing for standalone runner correctness.

## Testing Targets

Existing tests likely affected:

- `test_facedetections_artifacts`
- `test_facedetections_flow`
- `test_facedetections_tracking`
- `test_facedetections_processed_artifact`
- `test_facedetections_preview_smoke`
- `test_direct_vulkan_handoff_pipeline_contract`
- Any harnesses that invoke `runVulkanFacestreamOffscreen(...)`

Add focused tests where currently missing:

- Checkpoint resume index round trip.
- Runtime tuning JSON round trip.
- Detection sanitization edge cases.
- CLI parse coverage for zero-copy, workers, SCRFD variant, benchmark slots, and preview/control flags.

## Risks

- Concurrency: decoder-direct worker slots and futures must preserve frame-order result consumption.
- Vulkan lifetime: `VulkanHarnessContext`, handoff resources, and detector instances must not be destroyed while uploads/inference are pending.
- Resume compatibility: old `.part` files and compact resume indexes must still load.
- Summary schema: downstream tools may expect exact field names.
- Standalone source lists: missing a new `.cpp` in explicit CMake targets will produce link failures even if editor builds.

## Done Criteria

- `vulkan_facedetections_offscreen_main.cpp` is reduced to standalone entry-point glue.
- Runner orchestration is in `vulkan_facedetections_offscreen_runner.cpp`.
- No module other than runner owns unrelated responsibilities.
- Full build passes.
- Focused face-detection tests pass.
- A short frame-limited offscreen detection run produces the same summary/artifact schema as before.
