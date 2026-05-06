# VULKAN_PATH_PLAN (Goal: Native Vulkan + OpenGL Fallback)

## Primary Goal
Deliver a **native Vulkan preview path** with a **deterministic OpenGL fallback** policy.

Final meaning:
- `requested=vulkan` + `effective=vulkan` => true native Vulkan presentation/composition path.
- If Vulkan init/runtime fails => automatic OpenGL fallback with one clear reason.
- Bridge modes (`vulkan-cpu-present`, offscreen Vulkan-to-`QImage` presentation) are removed from the production preview path.

## Snapshot (as of 2026-05-05)

## Completed
- Backend preference plumbing exists and is persisted (`opengl`, `vulkan`, etc.).
- `PreviewSurface` abstraction added and integrated through core ownership paths.
- Construction-time backend selection is in place via preview surface factory.
- Structured backend decision logs implemented:
  - `[render-backend-selection] ...`
  - `[render-backend-fallback] ...`
- `vulkan_preview_surface.*` owns the direct `QVulkanWindow` swapchain presenter.
- The old parity bridge and offscreen `QImage` compositor are removed from the Vulkan preview path.
- Native Vulkan presentation is the default Vulkan preview attempt.
- The REST `/profile` preview payload reports `presenter=qvulkanwindow_direct_swapchain`, `swapchain_present=true`, `qimage_bridge=false`, and `qimage_materialized=false` when the direct presenter is active.
- Vulkan preview decode readiness is strict: `HardwareZeroCopy` mode rejects materialized CPU frames, so unsupported hardware reports missing GPU-ready frames instead of using `QImage`.
- The tracked duplicate `backup_preview/` implementation has been removed.
- Runtime reporting should reflect actual outcome:
  - default Vulkan path => direct swapchain presenter
  - init/runtime failure => OpenGL fallback with reason.

## Not Completed (Critical)
- Native Vulkan path does **not** yet render full decoded video frame composition parity (timeline video textures/overlays/interactions) independently.
- Current native path records direct swapchain commands and state/decode-readiness-driven clip rectangles, but does not yet import/upload/compose full timeline frame textures.
- Independent native compositor parity (without bridge) is not complete yet.
- Fallback logging consolidated to factory-level decision logging (single fallback line).

---

## Current Architecture State
- `PreviewSurface` is the shared interface used by editor orchestration.
- `VulkanPreviewSurface` currently does:
  - route through the shared preview interface
  - own a direct `QVulkanWindow` swapchain presenter
  - refuse the old QImage offscreen-composite bridge
  - fall back to OpenGL only if the native Vulkan presenter cannot be created
- OpenGL/legacy rendering logic still lives in `PreviewWindow` (`QOpenGLWidget` monolith).

---

## Remaining Work (Authoritative Checklist)

## Phase 1: Native Vulkan Surface Ownership (Finish Foundation)
- [x] Add `vulkan_preview_surface.*` scaffold.
- [x] Add native Vulkan surface init attempt (`QVulkanWindow`).
- [x] Remove reliance on hidden legacy delegate for core state/render path.
- [x] Ensure factory emits exactly one backend decision/fallback log line per startup.

### Exit Criteria
- Native path has its own execution path for state-to-render flow.
- No duplicate backend fallback logs.

## Phase 2: Vulkan Compositor Path (Core Rendering)
- [x] Implement Vulkan render loop for preview frames (swapchain-driven).
- [x] Connect decode/cache readiness metadata to the Vulkan presenter without accepting CPU `QImage` frames.
- [ ] Port frame upload/composition from OpenGL path to Vulkan resources/pipelines.
- [ ] Import CUDA/VAAPI hardware frames into Vulkan images, or provide an explicitly non-Vulkan fallback outside `effective=vulkan`.
- [ ] Connect timeline frame data path directly into Vulkan compositor.
- [ ] Validate resize/recreate/device-lost handling.

### Exit Criteria
- Native Vulkan mode renders visual clip frames without OpenGL rendering dependency.

## Phase 3: Overlay + Interaction Parity
- [ ] Port transcript/title/speaker/corrections overlays to the direct Vulkan presenter.
- [ ] Port hit-testing/drag/resize/edit interaction behavior for overlays to shared state plus Vulkan presenter.
- [ ] Maintain correction draw mode + transcript/title interaction modes without a bridge.

### Exit Criteria
- Feature parity with OpenGL preview interactions for agreed workflows.

## Phase 4: Deterministic OpenGL Fallback Finalization
- [ ] Harden fallback for native Vulkan init failure, swapchain failure, and device loss.
- [ ] Ensure fallback keeps UI responsive and preserves editor state.
- [ ] Ensure fallback reason is user/support actionable.

### Exit Criteria
- Vulkan failures never crash or dead-end preview; OpenGL fallback always usable.

## Phase 5: Bridge Retirement
- [x] Remove `vulkan-cpu-present` bridge mode from normal path.
- [x] Remove parity bridge mode from the Vulkan preview path.
- [ ] Keep only final selection/fallback logging paths.

### Exit Criteria
- `effective=vulkan` means native compositor path only.
- No bridge mode required for production operation.

## Phase 6: Validation + Performance Gates
- [ ] Run parity harness on representative projects and archive artifacts.
- [ ] Run throughput comparisons and document variance windows.
- [ ] Run stress tests (resize storms, scrub bursts, long playback, reopen loops).

### Exit Criteria
- Parity and throughput evidence archived.
- No critical regressions vs OpenGL baseline.

---

## Files Already Added/Modified for This Effort
- `preview_surface.h`
- `preview_surface_factory.h/.cpp`
- `preview_widget_factory.h/.cpp` (compat path still present)
- `vulkan_preview_surface.h/.cpp`
- `preview_window_core.cpp` (bridge logging guard)
- `scripts/vulkan_parity_throughput.sh`

## Planned Additional Vulkan-Focused Files (Still To Implement)
- `vulkan_resources.h/.cpp`
- `vulkan_pipeline.h/.cpp`
- `vulkan_overlay.h/.cpp` (or merged with renderer)

---

## Known Issues / Risks To Address Tomorrow
1. Native Vulkan mode presents through a direct swapchain, but it is not full compositor parity.
2. Native renderer currently records state/decode-readiness-driven preview commands, not decoded timeline video texture composition.
3. Swapchain/device-loss fallback hardening is not implemented yet.
4. Ensure no hidden OpenGL dependency remains once compositor migration completes.

---

## Exact Next Steps (Resume Order)
1. Route clip frame upload/composition into Vulkan path; validate one-clip render correctness.
2. Add `vulkan_resources.*` + `vulkan_pipeline.*` for swapchain images, descriptors, and draw pipeline.
3. Implement resize/recreate + device-lost fallback handling.
4. Port overlays incrementally and validate each against screenshot parity.
5. Remove bridge mode after parity and fallback gates pass.
