# TODO_PRO_TRACKER

## Goal
Build a full **native C++ production face tracker** path (no Python, no Docker) using a hybrid pipeline:
- Detector: SCRFD/RetinaFace (ONNX)
- ReID: ArcFace embeddings (ONNX)
- Association: IoU + embedding similarity
- Smoothing: temporal stabilization
- Output: existing BoxStream schema

## 1) Scope And Acceptance
- [ ] Runs from UI with no Python runtime.
- [ ] Runs from UI with no Docker runtime.
- [ ] Produces current continuity BoxStream JSON schema (no downstream breakage).
- [ ] Meets or beats current local hybrid tracking stability on `nasreen.mp4`.
- [ ] CPU-first implementation works reliably.
- [ ] Optional GPU acceleration can be added without API/schema changes.

## 2) Runtime And Model Plumbing
- [ ] Add ONNX Runtime C++ dependency to CMake/build.
- [ ] Add model asset management for:
  - [ ] Face detector ONNX (SCRFD/RetinaFace)
  - [ ] Face embedding ONNX (ArcFace)
- [ ] Decide model distribution strategy:
  - [ ] In-repo assets under `external/models/`, or
  - [ ] First-run download + checksum validation
- [ ] Add settings for model paths and thresholds.

## 3) C++ Inference Wrappers
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

## 4) Hybrid Tracker Core
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

## 5) Temporal Smoothing
- [ ] Add smoothing for center/scale:
  - [ ] EMA baseline
  - [ ] Optional Kalman path (future switch)
- [ ] Expose smoothing strength in config.
- [ ] Ensure smoothing does not drift off-face on cuts/occlusion.

## 6) BoxStream Integration
- [x] Add new detector preset in `BoxstreamDetectorPreset`:
  - [x] `NativeHybridCpu`
  - [ ] (future) `NativeHybridGpu`
- [x] Route `onSpeakerRunAutoTrackClicked()` to native hybrid branch.
- [x] Write `source` marker (e.g. `native_hybrid_v1`) in keyframes.
- [x] Keep serialized structure compatible with existing preview/assignment flow.

## 7) UI And Progress Visibility
- [x] Add preflight option: `Native Production Hybrid (C++)`.
- [x] Show explicit pipeline stages in run dialog:
  - [x] Detect
  - [x] Associate
  - [x] Smooth
  - [x] Write
  - [x] Preview
- [x] Keep preview tracker-source selector + legend support for native source.

## 8) Benchmark And Validation
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

## 9) Rollout Strategy
- [ ] Phase 1: ship behind feature flag.
- [ ] Phase 2: enable by default for CPU.
- [ ] Keep Python hybrid as fallback until parity confirmed.
- [ ] Remove fallback only after regression-free soak period.

## 10) Documentation
- [ ] Update `FACE_TRACKING_FLOW.md` with native hybrid flow.
- [ ] Update `DATAFLOW.md` with runtime/model dependency diagram.
- [ ] Add developer setup notes for ONNX runtime and model assets.
- [ ] Add troubleshooting section (model missing, CPU perf, association tuning).

## 11) Suggested Milestones
- [ ] Week 1: ONNX runtime + model plumbing + detector/embedder wrappers.
- [ ] Week 2: tracker association + smoothing + BoxStream write path.
- [ ] Week 3: UI stage reporting + benchmark integration + tuning.
- [ ] Week 4: stabilization, docs, default switch.
