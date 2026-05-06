# TODO

_Last updated: 2026-05-05_

## Current State
- Main app builds: `cmake --build build --target jcut -j$(nproc)`.
- Focused regressions pass: `ctest --test-dir build -R 'test_integration|test_vulkan_subtitle_render' --output-on-failure`.
- Native `QVulkanWindow` path owns a direct swapchain presenter and records Vulkan commands without a `QImage` bridge.
- REST `/profile` preview diagnostics verified: `presenter=qvulkanwindow_direct_swapchain`, `swapchain_present=true`, `qimage_bridge=false`, `qimage_materialized=false`.
- Vulkan preview now uses strict hardware-zero-copy decode readiness: CPU `QImage` frame materialization is rejected instead of being treated as a Vulkan-presentable frame.
- Native Vulkan face path exists and runs GPU preprocessing plus native Vulkan heuristic candidate inference. It is not a production model-backed detector yet.
- Standalone Vulkan face path reports GPU inference candidates on `nasreen.mp4`.
- Decode is still not end-to-end zero-copy: current benchmark decode emits CPU frames before Vulkan upload.

## Resume Next
1. Move full clip texture upload/composition into the direct presenter.
2. Add/import a hardware-frame-to-`VkImage` handoff for CUDA/VAAPI frames; unsupported systems should continue reporting `ready_decode_status_clips=0`, not fall back to `QImage`.
3. Keep Vulkan frame presentation in swapchain-owned resources; do not reintroduce `QImage` or CPU readback in the Vulkan preview path.
4. Validate one-clip render correctness, then resize/scrub/playback stress.

## Native Vulkan Preview And Compositor
- [ ] Finish the native swapchain-driven Vulkan compositor so `effective=vulkan` means native Vulkan composition, not bridge/parity mode.
  - Started: native `QVulkanWindow` now owns direct swapchain presentation and records frame commands without `QImage`.
  - Remaining: move full clip texture upload/composition into the direct presenter.
- [ ] Route clip frame upload/composition directly through Vulkan resources and pipelines.
- [ ] Connect timeline frame data directly into the native Vulkan compositor.
- [x] Connect Vulkan preview to decode/cache readiness without accepting CPU `QImage` frames as Vulkan-presentable.
- [ ] Harden resize, swapchain recreation, and device-loss handling.
- [ ] Harden OpenGL fallback for Vulkan init failure, swapchain failure, and device loss.
- [x] Remove `vulkan-cpu-present` / parity bridge from the production preview path.
- [x] Remove tracked duplicate `backup_preview/` preview implementation.
- [ ] Run parity and throughput validation against representative projects.

## Decode To Vulkan Zero-Copy
- [ ] Add hardware decode handoff that exposes decoded frames as Vulkan images instead of CPU `QImage` frames.
- [ ] Preserve proxy fallback while keeping the selected decode path observable in logs/diagnostics.
- [ ] Make `jcut_vulkan_zero_copy_video_preprocess --require-zero-copy` pass on supported hardware.
- [ ] Add diagnostics that separately report decode zero-copy, preprocess zero-copy, inference zero-copy, and end-to-end zero-copy.

## Native Vulkan Face Inference
- [ ] Replace the current native Vulkan heuristic face-candidate shader with a production model-backed Vulkan inference path.
- [ ] Decide production model target: full Res10 SSD Caffe graph port, ONNX/SCRFD graph, or a purpose-built compact Vulkan model.
- [ ] Implement/choose model asset loading, versioning, and checksum validation.
- [ ] Implement Vulkan kernels/runtime coverage for required model operations.
- [ ] Add GPU NMS or equivalent candidate suppression before CPU readback.
- [ ] Tune thresholds and candidate limits on `nasreen.mp4` plus crowded scenes.
- [ ] Add quality metrics: detection count, false-positive proxy, track length, ID-switch proxy, and runtime.

## FaceStream Native Tracking
- [ ] Extend the headless benchmark to include native Vulkan FaceStream inference.
- [ ] Compare native Vulkan against Python hybrid, Docker hybrid, Haar, YOLO, and RetinaFace paths.
- [ ] Stabilize native Vulkan tracks with association tuning and smoothing.
- [ ] Keep FaceStream schema compatibility while adding source metadata for native Vulkan inference.
- [ ] Update UI copy once native Vulkan inference is production-grade rather than heuristic.

## Native C++ Production Tracker
- [ ] Add ONNX Runtime C++ dependency or an equivalent native model runtime if ONNX is selected.
- [ ] Add model asset management for face detector and face embedding models.
- [ ] Implement native detector wrapper with preprocess, inference, decode, and NMS.
- [ ] Implement native embedder wrapper with crop/alignment, inference, and normalized embeddings.
- [ ] Implement hybrid tracker state, association cost, Hungarian assignment, birth/death policy, and smoothing.
- [ ] Validate native hybrid tracking on `nasreen.mp4` and at least one additional crowded scene.

## Documentation
- [ ] Update `VULKAN_PATH_PLAN.md` to match the current native Vulkan heuristic inference and remaining compositor work.
- [ ] Update `FACE_TRACKING_FLOW.md` with native Vulkan and native C++ tracker flow.
- [ ] Update developer setup docs for model assets and Vulkan diagnostics.
- [ ] Document fallback behavior and how to interpret zero-copy diagnostics.

## Verification Commands
```bash
cmake --build build --target jcut jcut_vulkan_zero_copy_video_preprocess -j$(nproc)
./build/jcut_vulkan_zero_copy_video_preprocess nasreen.mp4 --max-frames 48 --stride 12
ctest --test-dir build -R 'test_integration|test_vulkan_subtitle_render' --output-on-failure
```

Expected standalone Vulkan face output should include nonzero `detections=` lines and:
```text
preprocess_zero_copy=1
decode_zero_copy=0
end_to_end_zero_copy=0
```

## Files To Look At First
- `vulkan_preview_surface.cpp`: direct `QVulkanWindow` swapchain presenter; no `QImage` bridge.
- `vulkan_boxstream_offscreen_main.cpp`: default zero-copy Vulkan FaceStream verifier, with materialized compatibility mode behind `--materialized-generate-boxstream`.
- `vulkan_zero_copy_face_detector.cpp`: Vulkan preprocessing + heuristic inference compute pipelines.
- `speakers_tab_boxstream_actions.cpp`: UI integration for native Vulkan FaceStream inference.
