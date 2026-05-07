# REMAINING_WORK

_Last consolidated: 2026-05-06_

## Section A: Vulkan + Tracker Master TODO (from `TODO.md`)

_Last updated: 2026-05-05_

### Current State
- Main app builds: `cmake --build build --target jcut -j$(nproc)`.
- Focused regressions pass: `ctest --test-dir build -R 'test_integration|test_vulkan_subtitle_render' --output-on-failure`.
- Native `QVulkanWindow` path owns a direct swapchain presenter and records Vulkan commands without a `QImage` bridge.
- REST `/profile` preview diagnostics verified: `presenter=qvulkanwindow_direct_swapchain`, `swapchain_present=true`, `qimage_bridge=false`, `qimage_materialized=false`.
- Vulkan preview now uses strict hardware-zero-copy decode readiness: CPU `QImage` frame materialization is rejected instead of being treated as a Vulkan-presentable frame.
- Native Vulkan face path exists and runs GPU preprocessing plus native Vulkan heuristic candidate inference. It is not a production model-backed detector yet.
- Standalone Vulkan face path reports GPU inference candidates on `nasreen.mp4`.
- Decode is still not end-to-end zero-copy: current benchmark decode emits CPU frames before Vulkan upload.

### Resume Next
1. Move full clip texture upload/composition into the direct presenter.
2. Add/import a hardware-frame-to-`VkImage` handoff for CUDA/VAAPI frames; unsupported systems should continue reporting `ready_decode_status_clips=0`, not fall back to `QImage`.
3. Keep Vulkan frame presentation in swapchain-owned resources; do not reintroduce `QImage` or CPU readback in the Vulkan preview path.
4. Validate one-clip render correctness, then resize/scrub/playback stress.

### Native Vulkan Preview And Compositor
- [ ] Finish the native swapchain-driven Vulkan compositor so `effective=vulkan` means native Vulkan composition, not bridge/parity mode.
  - Started: native `QVulkanWindow` now owns direct swapchain presentation and records frame commands without `QImage`.
  - Remaining: move full clip texture upload/composition into the direct presenter.
- [ ] Route clip frame upload/composition directly through Vulkan resources and pipelines.
- [ ] Connect timeline frame data directly into the native Vulkan compositor.
- [ ] Harden resize, swapchain recreation, and device-loss handling.
- [ ] Harden OpenGL fallback for Vulkan init failure, swapchain failure, and device loss.
- [ ] Run parity and throughput validation against representative projects.

### Decode To Vulkan Zero-Copy
- [ ] Add hardware decode handoff that exposes decoded frames as Vulkan images instead of CPU `QImage` frames.
- [ ] Preserve proxy fallback while keeping the selected decode path observable in logs/diagnostics.
- [ ] Make `jcut_vulkan_zero_copy_video_preprocess --require-zero-copy` pass on supported hardware.
- [ ] Add diagnostics that separately report decode zero-copy, preprocess zero-copy, inference zero-copy, and end-to-end zero-copy.

### Native Vulkan Face Inference
- [ ] Replace the current native Vulkan heuristic face-candidate shader with a production model-backed Vulkan inference path.
- [ ] Decide production model target: full Res10 SSD Caffe graph port, ONNX/SCRFD graph, or a purpose-built compact Vulkan model.
- [ ] Implement/choose model asset loading, versioning, and checksum validation.
- [ ] Implement Vulkan kernels/runtime coverage for required model operations.
- [ ] Add GPU NMS or equivalent candidate suppression before CPU readback.
- [ ] Tune thresholds and candidate limits on `nasreen.mp4` plus crowded scenes.
- [ ] Add quality metrics: detection count, false-positive proxy, track length, ID-switch proxy, and runtime.

### FaceStream Native Tracking
- [x] Extend the headless benchmark to include native Vulkan FaceStream inference.
- [x] Add live tuning for native Vulkan FaceStream threshold, stride, candidate caps, ROI, area, and aspect filters.
- [x] Throttle preview/debug readback separately from detector frame processing with `--preview-stride`.
- [ ] Compare native Vulkan against Python hybrid, Docker hybrid, Haar, YOLO, and RetinaFace paths.
- [ ] Stabilize native Vulkan tracks with association tuning and smoothing.
- [ ] Keep FaceStream schema compatibility while adding source metadata for native Vulkan inference.
- [ ] Update UI copy once native Vulkan inference is production-grade rather than heuristic.

### Native C++ Production Tracker
- [ ] Add ONNX Runtime C++ dependency or an equivalent native model runtime if ONNX is selected.
- [ ] Add model asset management for face detector and face embedding models.
- [ ] Implement native detector wrapper with preprocess, inference, decode, and NMS.
- [ ] Implement native embedder wrapper with crop/alignment, inference, and normalized embeddings.
- [ ] Implement hybrid tracker state, association cost, Hungarian assignment, birth/death policy, and smoothing.
- [ ] Validate native hybrid tracking on `nasreen.mp4` and at least one additional crowded scene.

### Documentation
- [x] Keep this file in sync with native Vulkan heuristic inference and remaining compositor work.
- [ ] Update `FACE_TRACKING_FLOW.md` with native Vulkan and native C++ tracker flow.
- [ ] Update developer setup docs for model assets and Vulkan diagnostics.
- [x] Document fallback behavior and how to interpret zero-copy diagnostics.

### Verification Commands
```bash
cmake --build build --target jcut jcut_vulkan_boxstream_offscreen jcut_vulkan_zero_copy_video_preprocess -j$(nproc)
./build/jcut_vulkan_zero_copy_video_preprocess nasreen.mp4 --max-frames 48 --stride 12
./build/jcut_vulkan_boxstream_offscreen input.mp4 --max-frames 360 --params-file /tmp/jcut_runtime_params.json --no-preview-window --no-preview-files
ctest --test-dir build -R 'test_integration|test_vulkan_subtitle_render' --output-on-failure
```

Expected standalone Vulkan face output should include nonzero `detections=` lines and:
```text
preprocess_zero_copy=1
decode_zero_copy=0
end_to_end_zero_copy=0
```

### Files To Look At First
- `vulkan_preview_surface.cpp`: direct `QVulkanWindow` swapchain presenter; no `QImage` bridge.
- `vulkan_boxstream_offscreen_main.cpp`: default zero-copy Vulkan FaceStream verifier, with materialized compatibility mode behind `--materialized-generate-boxstream`.
- `vulkan_zero_copy_face_detector.cpp`: Vulkan preprocessing + heuristic inference compute pipelines.
- `speakers_tab_boxstream_actions.cpp`: UI integration for native Vulkan FaceStream inference.

---

## Section B: Native C++ Production Tracker Detail (from `TODO_PRO_TRACKER.md`)

### Goal
Build a full **native C++ production face tracker** path (no Python, no Docker) using a hybrid pipeline:
- Detector: SCRFD/RetinaFace (ONNX)
- ReID: ArcFace embeddings (ONNX)
- Association: IoU + embedding similarity
- Smoothing: temporal stabilization
- Output: existing FaceStream schema

### 1) Scope And Acceptance
- [ ] Runs from UI with no Python runtime.
- [ ] Runs from UI with no Docker runtime.
- [ ] Produces current continuity FaceStream JSON schema (no downstream breakage).
- [ ] Meets or beats current local hybrid tracking stability on `nasreen.mp4`.
- [ ] CPU-first implementation works reliably.
- [ ] Optional GPU acceleration can be added without API/schema changes.

### 2) Runtime And Model Plumbing
- [ ] Add ONNX Runtime C++ dependency to CMake/build.
- [ ] Add model asset management for:
  - [ ] Face detector ONNX (SCRFD/RetinaFace)
  - [ ] Face embedding ONNX (ArcFace)
- [ ] Decide model distribution strategy:
  - [ ] In-repo assets under `external/models/`, or
  - [ ] First-run download + checksum validation
- [ ] Add settings for model paths and thresholds.

### 3) C++ Inference Wrappers
- [ ] Implement `FaceDetectorOnnx`:
  - [ ] Preprocess
  - [ ] Inference
  - [ ] Decode predictions
  - [ ] NMS
- [ ] Implement `FaceEmbedderOnnx`:
  - [ ] Crop/alignment from detector boxes
  - [ ] Inference
  - [ ] L2-normalized embeddings
- [ ] Keep wrappers unit-testable (pure helper methods where possible).

### 4) Hybrid Tracker Core
- [ ] Implement `HybridFaceTracker` class with track state:
  - [ ] ID, bbox, velocity, last frame
  - [ ] embedding centroid/history
  - [ ] age/miss counters
- [ ] Build association cost:
  - [ ] IoU component
  - [ ] cosine similarity component
  - [ ] weighted fusion + gating
- [ ] Run Hungarian assignment per frame.
- [ ] Implement track birth/death policy.

### 5) Temporal Smoothing
- [ ] Add smoothing for center/scale:
  - [ ] EMA baseline
  - [ ] Optional Kalman path (future switch)
- [ ] Expose smoothing strength in config.
- [ ] Ensure smoothing does not drift off-face on cuts/occlusion.

### 6) FaceStream Integration
- [ ] (future) Add `NativeHybridGpu` preset in `BoxstreamDetectorPreset`.

### 7) Benchmark And Validation
- [ ] Extend headless benchmark to include native hybrid.
- [ ] Report metrics:
  - [ ] runtime
  - [ ] tracks
  - [ ] detections
  - [ ] avg/max track length
  - [ ] ID-switch proxy metric
- [ ] Compare against:
  - [ ] local Python hybrid
  - [ ] docker hybrid
  - [ ] current Haar/YOLO/RetinaFace options
- [ ] Validate on `nasreen.mp4` and at least 1 additional crowded scene.

### 8) Rollout Strategy
- [ ] Phase 1: ship behind feature flag.
- [ ] Phase 2: enable by default for CPU.
- [ ] Keep Python hybrid as fallback until parity confirmed.
- [ ] Remove fallback only after regression-free soak period.

### 9) Documentation
- [ ] Update `FACE_TRACKING_FLOW.md` with native hybrid flow.
- [ ] Update `DATAFLOW.md` with runtime/model dependency diagram.
- [ ] Add developer setup notes for ONNX runtime and model assets.
- [ ] Add troubleshooting section (model missing, CPU perf, association tuning).

### 10) Suggested Milestones
- [ ] Week 1: ONNX runtime + model plumbing + detector/embedder wrappers.
- [ ] Week 2: tracker association + smoothing + FaceStream write path.
- [ ] Week 3: UI stage reporting + benchmark integration + tuning.
- [ ] Week 4: stabilization, docs, default switch.

---

## Section C: Vulkan Path Plan Detail (from `VULKAN_PATH_PLAN.md`)

### Primary Goal
Deliver a **native Vulkan preview path** with a **deterministic OpenGL fallback** policy.

Final meaning:
- `requested=vulkan` + `effective=vulkan` => true native Vulkan presentation/composition path.
- If Vulkan init/runtime fails => automatic OpenGL fallback with one clear reason.
- Bridge modes (`vulkan-cpu-present`, offscreen Vulkan-to-`QImage` presentation) are removed from the production preview path.

### Snapshot (as of 2026-05-05)

### Not Completed (Critical)
- Native Vulkan path does **not** yet render full decoded video frame composition parity (timeline video textures/overlays/interactions) independently.
- Current native path records direct swapchain commands and state/decode-readiness-driven clip rectangles, but does not yet import/upload/compose full timeline frame textures.
- Independent native compositor parity (without bridge) is not complete yet.
- Fallback logging consolidated to factory-level decision logging (single fallback line).

### Current Architecture State
- `PreviewSurface` is the shared interface used by editor orchestration.
- `VulkanPreviewSurface` currently does:
  - route through the shared preview interface
  - own a direct `QVulkanWindow` swapchain presenter
  - refuse the old QImage offscreen-composite bridge
  - fall back to OpenGL only if the native Vulkan presenter cannot be created
- OpenGL/legacy rendering logic still lives in `PreviewWindow` (`QOpenGLWidget` monolith).

### Remaining Work (Authoritative Checklist)

### Phase 1: Native Vulkan Surface Ownership (Finish Foundation)

#### Exit Criteria
- Native path has its own execution path for state-to-render flow.
- No duplicate backend fallback logs.

### Phase 2: Vulkan Compositor Path (Core Rendering)
- [ ] Port frame upload/composition from OpenGL path to Vulkan resources/pipelines.
- [ ] Import CUDA/VAAPI hardware frames into Vulkan images, or provide an explicitly non-Vulkan fallback outside `effective=vulkan`.
- [ ] Connect timeline frame data path directly into Vulkan compositor.
- [ ] Validate resize/recreate/device-lost handling.

#### Exit Criteria
- Native Vulkan mode renders visual clip frames without OpenGL rendering dependency.

### Phase 3: Overlay + Interaction Parity
- [ ] Port transcript/title/speaker/corrections overlays to the direct Vulkan presenter.
- [ ] Port hit-testing/drag/resize/edit interaction behavior for overlays to shared state plus Vulkan presenter.
- [ ] Maintain correction draw mode + transcript/title interaction modes without a bridge.

#### Exit Criteria
- Feature parity with OpenGL preview interactions for agreed workflows.

### Phase 4: Deterministic OpenGL Fallback Finalization
- [ ] Harden fallback for native Vulkan init failure, swapchain failure, and device loss.
- [ ] Ensure fallback keeps UI responsive and preserves editor state.
- [ ] Ensure fallback reason is user/support actionable.

#### Exit Criteria
- Vulkan failures never crash or dead-end preview; OpenGL fallback always usable.

### Phase 5: Bridge Retirement
- [ ] Keep only final selection/fallback logging paths.

#### Exit Criteria
- `effective=vulkan` means native compositor path only.
- No bridge mode required for production operation.

### Phase 6: Validation + Performance Gates
- [ ] Run parity harness on representative projects and archive artifacts.
- [ ] Run throughput comparisons and document variance windows.
- [ ] Run stress tests (resize storms, scrub bursts, long playback, reopen loops).

#### Exit Criteria
- Parity and throughput evidence archived.
- No critical regressions vs OpenGL baseline.

### Files Already Added/Modified for This Effort
- `preview_surface.h`
- `preview_surface_factory.h/.cpp`
- `preview_widget_factory.h/.cpp` (compat path still present)
- `vulkan_preview_surface.h/.cpp`
- `preview_window_core.cpp` (bridge logging guard)
- `scripts/vulkan_parity_throughput.sh`

### Planned Additional Vulkan-Focused Files (Still To Implement)
- `vulkan_resources.h/.cpp`
- `vulkan_pipeline.h/.cpp`
- `vulkan_overlay.h/.cpp` (or merged with renderer)

### Known Issues / Risks To Address Tomorrow
1. Native Vulkan mode presents through a direct swapchain, but it is not full compositor parity.
2. Native renderer currently records state/decode-readiness-driven preview commands, not decoded timeline video texture composition.
3. Swapchain/device-loss fallback hardening is not implemented yet.
4. Ensure no hidden OpenGL dependency remains once compositor migration completes.

### Exact Next Steps (Resume Order)
1. Route clip frame upload/composition into Vulkan path; validate one-clip render correctness.
2. Add `vulkan_resources.*` + `vulkan_pipeline.*` for swapchain images, descriptors, and draw pipeline.
3. Implement resize/recreate + device-lost fallback handling.
4. Port overlays incrementally and validate each against screenshot parity.
5. Remove bridge mode after parity and fallback gates pass.
