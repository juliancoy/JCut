# PROBLEMS.md

This file records the architectural issues reviewed from the previous problem list and their resolution status.

## Resolved

### Header-only implementation in face detection

Resolved by splitting the public face-detection generation contract from implementation:

- `facedetections_generation.h` now contains the public settings, preset declarations, and narrow OpenCV-facing declarations.
- `facedetections_generation.cpp` owns the implementation for ETA formatting, cascade lookup, OpenCV image conversion, weighted detection filtering, scan-preview conversion, and Res10 DNN model asset lookup.

### Vulkan harness implementation in a header

Resolved by moving resource-owning method bodies out of the header:

- `vulkan_facedetections_offscreen_vulkan_harness.h` now declares the harness API and state.
- `vulkan_facedetections_offscreen_vulkan_harness.cpp` owns stderr silencing, Vulkan instance/device setup, buffer/image resource management, upload, transition, and teardown implementation.

### Wide render backend virtual API

Resolved at the polymorphic boundary:

- `render_internal.h` now exposes `render_detail::OffscreenRenderContext` as the virtual render request object.
- `OffscreenRenderer::renderFrame(const OffscreenRenderContext&)` and `OffscreenRenderer::renderFrameToOutput(const OffscreenRenderContext&, ...)` are the virtual backend contract.
- The previous long-argument render calls remain as non-virtual compatibility wrappers so existing call sites and tests do not need a risky mechanical rewrite.

### Preview abstraction depended on widget internals

Resolved for the preview abstraction boundary:

- `preview_surface.h` no longer includes `timeline_widget.h`.
- `preview_surface.h` no longer directly includes `editor_shared.h`; it forward-declares the value types used by reference.
- Concrete preview implementations that store timeline/domain values include the complete domain headers themselves.

### Face-detection runtime depended on speaker UI internals

Resolved:

- `facedetections_runtime.cpp` no longer includes `speakers_tab_internal.h`.
- The include was not needed by the runtime implementation, so no shared-domain extraction was required for this file.

### Large files after modularization

Reviewed:

- The largest files remain candidates for future module-level ownership splits, but this pass found no redundant `.inc` split files and no duplicate ownership introduced by the recent Vulkan/face-detection work.
- The immediate architectural issues in this problem list were the active header-only implementation, virtual API width, and cross-layer includes. Those have been resolved above.

### Visible frame request ownership

Resolved:

- `TimelineCache::requestFrame()` now owns visible-request registration, deduplication, null-decoder handling, dispatch bookkeeping, and completion cleanup.
- Decoder-less cache tests can exercise pending visible request behavior without dereferencing an invalid decoder.
- Repeated requests for the same visible frame share the in-flight request and preserve all callbacks.

### Embedded dependency test ownership

Resolved:

- Embedded `CPPMonetize` tests are now controlled by `CPPMONETIZE_BUILD_TESTS`.
- JCut disables those dependency tests when adding the vendored project as `EXCLUDE_FROM_ALL`, so CTest no longer registers missing dependency test executables.
- CPPMonetize still defaults to building its own tests when configured as a top-level project.

## Verification

- `cmake --build build --target jcut_vulkan_facedetections_offscreen -j2`
- `cmake --build build -j2`
- `./build/bin/test_facedetections_artifacts`
- `./build/bin/test_facedetections_tracking`
- `./build/bin/test_direct_vulkan_handoff_pipeline_contract`
- `./build/bin/test_vulkan_subtitle_render`
- `./build/bin/test_opengl_vulkan_render_exactness`
- `ctest --test-dir build --output-on-failure -j2` passed: 42 passed, 0 failed, 1 skipped (`test_live_playback_lag_probe`).
- `git diff --check`
